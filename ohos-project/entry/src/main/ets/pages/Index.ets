import entryNapi from 'libentry.so';
import { common, wantAgent } from '@kit.AbilityKit';
import { backgroundTaskManager } from '@kit.BackgroundTasksKit';
import { fileIo as fs, picker } from '@kit.CoreFileKit';
import { http } from '@kit.NetworkKit';
import zlib from '@ohos.zlib';
import { util } from '@kit.ArkTS';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { promptAction, window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

/** data-assets.json entry. */
interface DataAsset {
  name: string;
  size: number;
  sha256: string;
  packs: string[];
  fileCount: number;
}

/** default-manifest.json (build-time injected download root). */
interface DefaultManifest {
  baseUrl: string;
  releaseTag: string;
}

/** data-assets.json root, shipped as rawfile by the CI workflow. */
interface DataManifest {
  schemaVersion: number;
  releaseTag: string;
  baseUrl: string;
  assets: DataAsset[];
}

// The app-owned public Download folder: Download/<bundleName>. On HarmonyOS
// NEXT this exact per-app folder is writable WITHOUT any permission
// (unlike arbitrary paths under Download), and shows under the app name in
// the file manager. The system creates it automatically.
const APP_FOLDER = 'com.lightwinder.yosuganosora.hdremake';
const DOWNLOAD_ROOT = '/storage/Users/currentUser/Download';

@Entry
@Component
struct Index {
  // 0 checking, 1 ready, 2 need data, 3 downloading, 4 importing, 5 error, 6 running
  @State bootState: number = 0;
  @State progressText: string = '';
  @State progressValue: number = 0;
  @State baseUrl: string = '';
  @State proxyUrl: string = '';
  @State message: string = '';
  @State needInit: boolean = false;
  @State uiEpoch: number = 0;
  @State debugOut: string = '';
  private autoInitTried: boolean = false;
  @State readyForGame: boolean = false;
  private sandboxFiles: string = '';
  private baseDir: string = '';
  private saveDir: string = '';
  private appDir: string = '';
  private dataDir: string = '';
  private assets: DataAsset[] = [];
  private manifest: DataManifest | null = null;
  private xcController: XComponentController = new XComponentController();
  private xcVideoController: XComponentController = new XComponentController();
  // True while the AVPlayer renders video: the video XComponent is raised
  // above the game XComponent (polled from native, no NAPI callback needed).
  @State videoOnTop: boolean = false;
  // Background keep-alive: a continuous audio-playback task keeps the game
  // process (and BGM) alive while the user switches away from the app.
  private keepAliveStarted: boolean = false;
  private keepAliveWantAgent: wantAgent.WantAgent | null = null;

  aboutToAppear(): void {
    this.sandboxFiles = (getContext(this) as common.UIAbilityContext).filesDir;
    // Raise the video XComponent above the game XComponent while the AVPlayer
    // plays; poll the native flag (100ms) instead of a NAPI callback thread.
    setInterval(() => {
      try {
        this.videoOnTop = entryNapi.isVideoPlaying();
        // The in-game exit button finishes the engine thread; the native
        // side reports "running" until the engine has actually started, so
        // a 0 here only ever means the game really quit. Terminate the app
        // (mirrors the Windows exit-button behaviour) instead of leaving a
        // frozen black XComponent on screen.
        if (this.bootState === 6 && !entryNapi.isEngineRunning()) {
          this.log('engine finished; terminating the app');
          this.stopKeepAlive();
          try {
            (getContext(this) as common.UIAbilityContext).terminateSelf();
          } catch (eTerm) {
            // best effort: fall back to the bootstrap page
            this.bootState = 2;
            this.progressText = '游戏数据已就绪';
            this.readyForGame = true;
            this.uiEpoch++;
          }
        }
      } catch (e) {
        // entryNapi not ready yet - keep the current value
      }
    }, 100);
    // Show the bootstrap screen immediately with ZERO filesystem or
    // permission work, so no platform call can stall the UI thread before
    // the user sees it. Data checks run on demand below.
    this.bootState = 2;
    this.progressText = '需要下载或导入游戏数据';
  }

  // Capture ArkTS page errors so UI-thread crashes do not wedge the page.
  onPageError(err: Error): void {
    try {
      this.log('PAGE_ERROR: ' + JSON.stringify(err));
    } catch (e) {
      hilog.error(0x0000, 'YosugaOHOS', 'PAGE_ERROR %{public}s', JSON.stringify(err));
    }
  }

  onPageShow(): void {
    // Rotate to landscape during the bootstrap page (no XComponent exists
    // yet) - rotating later, while the XComponent is mounted, crashes
    // libace_compatible on this system.
    setTimeout(() => {
      this.rotateToLandscape();
    }, 100);
    // Auto-run the data check / public-dir init shortly after the bootstrap
    // page is visible (avoid stalling startup).
    setTimeout(() => {
      this.prepare();
    }, 600);
    // Re-acquire the keep-alive task when returning from the background
    // (the system may have cancelled it while the app was hidden).
    if (this.bootState === 6) {
      this.startKeepAlive();
    }
  }

  async prepare(): Promise<void> {
    if (this.bootState === 6) {
      return; // already running
    }
    this.bootState = 0;
    this.progressText = '正在检查游戏数据…';
    try {
      await this.prepareInternal();
    } catch (e) {
      // Never crash the app on the bootstrap path: show the error instead.
      this.bootState = 5;
      this.message = '初始化失败：' + JSON.stringify(e);
      this.log('prepare failed: ' + JSON.stringify(e));
    }
  }

  private log(msg: string): void {
    hilog.info(0x0000, 'YosugaOHOS', '%{public}s', msg);
    // Mirror onto the on-screen debug area only.
    const line = Date.now() % 100000 + ': ' + msg;
    this.debugOut = (this.debugOut + '\n' + line).slice(-800);
  }

  /**
   * Start the background keep-alive task (continuous audio playback) so the
   * game keeps running while the user switches away from the app.
   */
  private async startKeepAlive(): Promise<void> {
    try {
      if (this.keepAliveStarted) {
        return;
      }
      const ctx = getContext(this) as common.UIAbilityContext;
      const wantAgentInfo: wantAgent.WantAgentInfo = {
        wants: [{
          bundleName: ctx.abilityInfo.bundleName,
          abilityName: 'EntryAbility'
        }],
        operationType: wantAgent.OperationType.START_ABILITY,
        requestCode: 0,
        wantAgentFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
      };
      this.keepAliveWantAgent = await wantAgent.getWantAgent(wantAgentInfo);
      await backgroundTaskManager.startBackgroundRunning(
        ctx,
        backgroundTaskManager.BackgroundMode.AUDIO_PLAYBACK,
        this.keepAliveWantAgent
      );
      this.keepAliveStarted = true;
    } catch (e) {
      // Keep-alive is best effort: the app must never die over it.
      this.log('startBackgroundRunning failed: ' + JSON.stringify(e));
    }
  }

  private async stopKeepAlive(): Promise<void> {
    try {
      if (!this.keepAliveStarted) {
        return;
      }
      await backgroundTaskManager.stopBackgroundRunning(
        getContext(this) as common.UIAbilityContext
      );
      this.keepAliveStarted = false;
    } catch (e) {
      // ignore
    }
  }

  prepareInternal(): void {
    this.log('prepareInternal begin');
    // NOTE: no permission dialog here. On HarmonyOS 7 (API 26) the system
    // permission dialog is a UIExtension whose close can wipe the host page
    // content (black screen). Permissions are requested on demand when the
    // user taps download/import instead.

    // Prefer the user-granted public Download/<bundleName> folder: after
    // initPublicDir() runs (DocumentViewPicker.save DOWNLOAD mode) the system
    // creates Download/<bundleName>/ and grants the app persistent access
    // without any media permission. Probe it; fall back to the sandbox.
    // The public Download/<bundleName>/ folder only becomes usable after
    // initPublicDir() (DocumentViewPicker.save DOWNLOAD mode) runs: the system
    // then creates it and grants persistent access. We self-check by looking
    // for a marker file we write there; do NOT blindly mkdir the public path
    // (that can appear to succeed yet be unwritable). Until granted, use the
    // sandbox so downloads/imports always work, and surface the init button.
    this.appDir = DOWNLOAD_ROOT + '/' + APP_FOLDER;
    // Only treat the public folder as usable when the marker we wrote after
    // initPublicDir() is actually listed there. listFileSync on a missing
    // folder throws, so an un-granted platform reliably falls into the
    // sandbox branch and shows the init button.
    let publicOk = false;
    try {
      const entries = fs.listFileSync(this.appDir);
      publicOk = entries.indexOf('.granted') >= 0;
      if (publicOk) {
        fs.mkdirSync(this.appDir + '/savedata', true);
      }
    } catch (e) {
      this.log('public app dir not granted: ' + JSON.stringify(e));
    }
    if (publicOk) {
      this.baseDir = this.appDir;
      this.saveDir = this.appDir + '/savedata';
      this.dataDir = this.appDir + '/data';
      this.needInit = false;
      this.message = '';
      this.log('data dir: public ' + this.appDir);
    } else {
      // Public folder not granted yet: request it automatically on first
      // launch (DocumentViewPicker.save DOWNLOAD mode). Until granted, use
      // the sandbox so downloads/imports always work.
      this.baseDir = this.sandboxFiles;
      this.saveDir = this.sandboxFiles + '/savedata';
      this.dataDir = this.sandboxFiles + '/data';
      this.needInit = false;
      this.message = '数据暂存于应用私有目录，可点击「初始化下载目录」改为系统下载目录';
      if (!this.autoInitTried) {
        this.autoInitTried = true;
        this.log('data dir: sandbox (public not granted), auto-initing once');
        this.initPublicDir();
      }
    }
    try {
      fs.mkdirSync(this.saveDir, true);
    } catch (e) {
      // already exists
    }
    const ready = this.dataReady();
    this.readyForGame = ready;
    this.log('dataReady=' + ready + ' dataDir=' + this.dataDir);
    // Do NOT auto-start (avoids landscape black screen); show a start
    // button when ready. Only fetch the download manifest when data is
    // missing and the user actually wants to download/import.
    if (ready) {
      this.bootState = 2;
      this.progressText = '游戏数据已就绪';
      this.message = '';
      this.assets = [];
    } else {
      this.bootState = 2;
      this.progressText = '需要下载或导入游戏数据';
    }
    this.uiEpoch++;
    this.log('bootstrap page shown');
  }

  /** Read the build-time-injected default base URL from rawfile
   *  default-manifest.json (contains the current repository's Release path).
   *  Returns '' when absent. */
  readDefaultBaseUrl(): string {
    try {
      const manager = (getContext(this) as common.UIAbilityContext).resourceManager;
      const bytes = manager.getRawFileContentSync('default-manifest.json');
      const text = new util.TextDecoder('utf-8').decodeWithStream(bytes);
      const parsed = JSON.parse(text) as DefaultManifest;
      return parsed.baseUrl || '';
    } catch (e) {
      return '';
    }
  }

  /** Fetch the data manifest from <baseUrl>data-assets.json so the app gets
   *  the real asset names/sizes/hashes without the build needing to bundle
   *  game data. baseUrl is user-editable (direct link or a proxy prefix). */
  async loadManifest(): Promise<void> {
    try {
      if (this.baseUrl.trim() === '') {
        // Default download root was injected at build time (so forks/renames
        // track the current repository); fall back to the project URL.
        this.baseUrl = this.readDefaultBaseUrl() ||
          'https://github.com/WarSkyGod/yosuga-no-sora-remake/releases/latest/download/';
      }
      const manifestUrl = this.baseUrl + 'data-assets.json';
      const req = http.createHttp();
      let parsed: DataManifest | null = null;
      try {
        const resp: http.HttpResponse = await req.request(manifestUrl, {
          method: http.RequestMethod.GET,
          expectDataType: http.HttpDataType.STRING,
          connectTimeout: 30000,
          readTimeout: 60000
        });
        if (resp.responseCode === 200) {
          parsed = JSON.parse(resp.result as string) as DataManifest;
        } else {
          this.message = '获取数据清单失败（HTTP ' + resp.responseCode + '），请检查下载地址';
        }
      } finally {
        req.destroy();
      }
      if (parsed !== null) {
        // Keep the user's base (direct or proxy); only adopt the bundled
        // release URL if the user left it empty.
        if (this.baseUrl.indexOf('github.com') < 0 && parsed.baseUrl) {
          this.baseUrl = parsed.baseUrl + '/';
        }
        this.manifest = parsed;
        this.assets = parsed.assets;
        this.message = '';
        this.log('manifest fetched, assets=' + this.assets.length);
      }
    } catch (e) {
      this.message = '获取数据清单失败：' + JSON.stringify(e);
      this.assets = [];
    }
  }

  /** Ask the user once to grant the app-owned Download/<bundleName> folder.
   *  documentViewPicker.save() with DOWNLOAD mode makes the system create
   *  Download/<bundleName>/ and grants persistent access without any media
   *  permission. Returns true when the public folder became usable. */
  async initPublicDir(): Promise<boolean> {
    this.message = '';
    try {
      const dp = new picker.DocumentViewPicker(getContext(this) as common.UIAbilityContext);
      const opts = new picker.DocumentSaveOptions();
      opts.pickerMode = picker.DocumentPickerMode.DOWNLOAD;
      opts.newFileNames = ['YosugaNoSoraHD.initialized'];
      this.log('initPublicDir: opening save (DOWNLOAD mode)');
      try {
        await dp.save(opts);
      } catch (e) {
        this.log('initPublicDir: save rejected/canceled: ' + JSON.stringify(e));
        this.message = '初始化下载目录未确认（可稍后在引导页点击「初始化下载目录」）';
        return false;
      }
      this.log('initPublicDir: save returned (user confirmed)');
      // The system created Download/<bundleName>/ and granted access.
      this.appDir = DOWNLOAD_ROOT + '/' + APP_FOLDER;
      this.log('initPublicDir: appDir=' + this.appDir);
      try {
        fs.mkdirSync(this.appDir + '/savedata', true);
      } catch (e) {
        // may already exist
      }
      // Write a marker that proves we have write access; prepare() relies on
      // it to decide whether to use the public folder on later launches.
      try {
        const marker = fs.openSync(this.appDir + '/.granted', fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
        fs.writeSync(marker.fd, 'granted');
        fs.closeSync(marker);
      } catch (e) {
        this.log('marker write failed: ' + JSON.stringify(e));
      }
      this.baseDir = this.appDir;
      this.saveDir = this.appDir + '/savedata';
      this.dataDir = this.appDir + '/data';
      this.needInit = false;
      this.log('public dir initialised: ' + this.appDir);
      // trace the data layout so we can see what the engine will find
      this.log('baseDir list: ' + JSON.stringify(this.tryList(this.baseDir)));
      this.log('dataDir list: ' + JSON.stringify(this.tryList(this.dataDir)));
      this.log('root data.xp3? ' + this.hasDirEntry(this.baseDir, 'data.xp3'));
      return true;
    } catch (e) {
      this.message = '初始化下载目录失败：' + JSON.stringify(e);
      this.log('initPublicDir failed: ' + JSON.stringify(e));
      return false;
    } finally {
      // The picker is a UIExtension; rebuild after it closes so the page is
      // not left blank.
      this.uiEpoch++;
    }
  }

  /** Use the app-owned public Download folder (Download/<bundleName>) if the
   *  platform allows it without permissions; otherwise keep the sandbox.
   *  Returns true when the public folder is usable. */
  async ensurePublic(): Promise<boolean> {
    // Never create this public path directly and never use accessSync here:
    // on this device accessSync reports nonexistent paths as present. The
    // .granted marker is only written after the system DOWNLOAD picker grants
    // this app-owned directory.
    const entries = this.tryList(this.appDir);
    if (entries.indexOf('.granted') < 0) {
      this.log('public app dir unavailable: marker missing; requesting grant');
      return await this.initPublicDir();
    }
    if (this.baseDir !== this.appDir) {
      this.baseDir = this.appDir;
      this.saveDir = this.appDir + '/savedata';
      this.dataDir = this.appDir + '/data';
      try {
        fs.mkdirSync(this.saveDir, true);
      } catch (e) {
        // already exists
      }
      this.message = '';
      this.log('switched to public dir: ' + this.baseDir);
    }
    return true;
  }

  async downloadAll(): Promise<void> {
    if (this.assets.length === 0) {
      await this.loadManifest();
    }
    if (this.assets.length === 0) {
      this.message = '下载清单为空，请检查网络或稍后重试';
      return;
    }
    this.bootState = 3;
    this.message = '';
    await this.ensurePublic();
    this.uiEpoch++; // force a rebuild after the permission dialog closes
    const cacheDir = this.sandboxFiles + '/cache';
    try {
      fs.mkdirSync(cacheDir, true);
    } catch (e) {
      // already exists
    }
    let doneBytes = 0;
    const totalBytes = this.assets.reduce((sum, asset) => sum + asset.size, 0);
    const startTime = Date.now();
    for (const asset of this.assets) {
      const originalUrl = this.baseUrl + '/' + asset.name;
      const proxy = this.proxyUrl.trim();
      const url = proxy !== '' ? proxy + originalUrl : originalUrl;
      this.progressText = '正在下载 ' + asset.name;
      const tmp = cacheDir + '/' + asset.name;
      try {
        await this.downloadFile(url, tmp, asset.size, (downloaded: number) => {
          const doneTotal = doneBytes + downloaded;
          const elapsed = Math.max(1, (Date.now() - startTime) / 1000);
          const speed = doneTotal / elapsed;
          const pct = Math.min(99, Math.floor(doneTotal * 100 / Math.max(1, totalBytes)));
          this.progressValue = pct;
          this.progressText = '正在下载 ' + asset.name + '  ' + pct + '%  ' +
            this.fmtSize(doneTotal) + ' / ' + this.fmtSize(totalBytes) +
            '  (' + this.fmtSpeed(speed) + ')';
          // Force a rebuild so the progress text is repainted even if the
          // state-driven refresh fails on this system version.
          this.uiEpoch++;
        });
        if (asset.sha256 !== '' && !(await this.verifySha256(tmp, asset.sha256))) {
          throw new Error('SHA-256 校验失败，请重试');
        }
      } catch (e) {
        this.bootState = 5;
        this.message = '下载失败：' + asset.name + '（' + JSON.stringify(e) + '）\n' +
          '若为网络问题，请返回引导页点击「gh-proxy」或「我的代理」切换加速后再下载';
        return;
      }
      await this.unzipTo(tmp, this.baseDir);
      try {
        fs.unlinkSync(tmp);
      } catch (e) {
        // ignore
      }
      doneBytes += asset.size;
      this.progressValue = Math.min(99, Math.floor(doneBytes * 100 / Math.max(1, totalBytes)));
      await this.delay(50);
    }
    await this.finishData();
  }

  async downloadFile(url: string, dest: string, totalSize: number,
    onProgress?: (downloaded: number) => void): Promise<void> {
    // Range-chunked download with resume + retry. GitHub release downloads
    // commonly drop mid-transfer; each chunk is retried a few times and
    // written at its exact offset so a retry continues where it left off.
    const file = fs.openSync(dest, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE);
    const chunkSize = 8 * 1024 * 1024;
    let offset = 0;
    try {
      while (offset < totalSize) {
        const end = Math.min(offset + chunkSize - 1, totalSize - 1);
        let got = false;
        for (let attempt = 0; attempt < 4 && !got; attempt++) {
          if (attempt > 0) {
            await this.delay(2000);
          }
          const httpRequest = http.createHttp();
          try {
            const response: http.HttpResponse = await httpRequest.request(url, {
              method: http.RequestMethod.GET,
              header: { 'Range': 'bytes=' + offset + '-' + end },
              expectDataType: http.HttpDataType.ARRAY_BUFFER,
              connectTimeout: 60000,
              readTimeout: 120000
            });
            if (response.responseCode === 206) {
              const data = response.result as ArrayBuffer;
              const length = data.byteLength;
              if (length > 0) {
                fs.writeSync(file.fd, data, { offset: offset });
                offset += length;
                if (onProgress) {
                  onProgress(offset);
                }
                got = true;
              }
            } else if (response.responseCode === 200) {
              // Server ignored the Range: write the whole body from the start.
              const data = response.result as ArrayBuffer;
              fs.truncateSync(file.fd, 0);
              fs.writeSync(file.fd, data, { offset: 0 });
              offset = totalSize;
              got = true;
              if (onProgress) {
                onProgress(totalSize);
              }
            } else if (response.responseCode === 416) {
              offset = totalSize;
              got = true;
            } else if (attempt === 3) {
              throw new Error('HTTP ' + response.responseCode);
            }
          } catch (e) {
            if (attempt === 3) {
              if (e instanceof Error) {
                throw e;
              }
              throw new Error(String(e));
            }
          } finally {
            httpRequest.destroy();
          }
        }
        if (!got && offset < totalSize) {
          throw new Error('下载中断，无法继续');
        }
        await this.delay(50);
      }
    } finally {
      fs.closeSync(file);
    }
  }

  async unzipTo(srcZip: string, outDir: string): Promise<void> {
    await zlib.decompressFile(srcZip, outDir);
  }

  /** True only when the Windows krkrz-style root contains data.xp3, or
   *  when a complete loose data tree exists in <base>/data/. */
  dataReady(): boolean {
    // Do not use accessSync: this device reports nonexistent paths as present.
    // listFileSync gives us the actual directory entries instead.
    const rootEntries = this.tryList(this.baseDir);
    const dataEntries = this.tryList(this.dataDir);
    const hasRootXp3 = rootEntries.indexOf('data.xp3') >= 0;
    const hasStartup = dataEntries.indexOf('startup.tjs') >= 0;
    const hasSystem = dataEntries.indexOf('system') >= 0;
    const hasLooseTree = hasStartup && hasSystem;
    this.log('dataReady: root=' + JSON.stringify(rootEntries).slice(0, 300) +
      ' data=' + JSON.stringify(dataEntries).slice(0, 300) +
      ' root/data.xp3=' + hasRootXp3 + ' loose-tree=' + hasLooseTree);
    return hasRootXp3 || hasLooseTree;
  }

  private tryList(path: string): string[] {
    try {
      return fs.listFileSync(path);
    } catch (e) {
      return ['<err:' + JSON.stringify(e) + '>'];
    }
  }

  private hasDirEntry(dir: string, name: string): boolean {
    return this.tryList(dir).indexOf(name) >= 0;
  }

  async verifySha256(path: string, expected: string): Promise<boolean> {
    const md = cryptoFramework.createMd('SHA256');
    const file = fs.openSync(path, fs.OpenMode.READ_ONLY);
    const chunk = new ArrayBuffer(1024 * 1024);
    try {
      while (true) {
        const len = fs.readSync(file.fd, chunk);
        if (len <= 0) {
          break;
        }
        const view = new Uint8Array(chunk.slice(0, len));
        await md.update({ data: view });
      }
    } finally {
      fs.closeSync(file);
    }
    const digest = await md.digest();
    const bytes = digest.data;
    let hex = '';
    for (let i = 0; i < bytes.length; i++) {
      hex += bytes[i].toString(16).padStart(2, '0');
    }
    return hex.toUpperCase() === expected.toUpperCase();
  }

  async showNotice(title: string, msg: string): Promise<void> {
    await promptAction.showDialog({
      title: title,
      message: msg,
      buttons: [{ text: '知道了', color: '#FFFFFF' }]
    });
  }

  /** Copy from a document picker uri (file:// or datashare://) to a path.
   * Plain copyFileSync fails on picker uris with error 13900002, and a
   * pre-existing destination triggers 13900015, so remove it first. */
  copyFromUri(uri: string, dest: string): void {
    try {
      fs.unlinkSync(dest);
    } catch (e) {
      // destination may not exist yet - fine
    }
    let src: fs.File | null = null;
    try {
      src = fs.openSync(uri, fs.OpenMode.READ_ONLY);
    } catch (e) {
      // Some systems hand out datashare:// uris that openSync rejects; the
      // copyFile helper understands more uri schemes.
      fs.copyFileSync(uri, dest);
      return;
    }
    const dst = fs.openSync(dest, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    const buf = new ArrayBuffer(1024 * 1024);
    try {
      while (true) {
        const n = fs.readSync(src.fd, buf);
        if (n <= 0) {
          break;
        }
        const chunk = buf.slice(0, n);
        fs.writeSync(dst.fd, chunk);
      }
    } finally {
      fs.closeSync(src);
      fs.closeSync(dst);
    }
  }

  fmtSize(bytes: number): string {
    if (bytes >= 1024 * 1024 * 1024) {
      return (bytes / (1024 * 1024 * 1024)).toFixed(2) + ' GB';
    }
    if (bytes >= 1024 * 1024) {
      return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
    }
    return Math.round(bytes / 1024) + ' KB';
  }

  fmtSpeed(bytesPerSec: number): string {
    return this.fmtSize(bytesPerSec) + '/s';
  }

  async importLocal(): Promise<void> {
    this.bootState = 4;
    this.message = '';
    await this.ensurePublic();
    this.uiEpoch++; // force a rebuild after the permission dialog closes
    const documentPicker = new picker.DocumentViewPicker(getContext(this) as common.UIAbilityContext);
    let uris: string[] = [];
    try {
      const result = await documentPicker.select({ maxSelectNumber: 20 });
      uris = result;
    } catch (e) {
      // User cancelled the picker: rebuild the whole bootstrap page so the
      // picker (also a UIExtension) cannot leave it blank.
      this.message = '未选择文件';
      this.prepare();
      return;
    }
    if (uris.length === 0) {
      this.message = '未选择文件';
      this.prepare();
      return;
    }
    // Force a rebuild after the picker (UIExtension) closes so the page is
    // not left blank, then continue importing.
    this.uiEpoch++;
    this.bootState = 4;
    const cacheDir = this.sandboxFiles + '/cache';
    try {
      fs.mkdirSync(cacheDir, true);
    } catch (e) {
      // already exists
    }
    for (let i = 0; i < uris.length; i++) {
      const uri = uris[i];
      const name = uri.substring(uri.lastIndexOf('/') + 1);
      this.progressText = '正在导入 ' + name;
      this.progressValue = Math.floor((i + 1) * 100 / Math.max(1, uris.length));
      const lower = name.toLowerCase();
      try {
        if (lower.endsWith('.xp3')) {
          // A bare data.xp3 goes straight onto the app root, beside savedata
          // (mirrors Windows krkrz: data.xp3 lives next to the executable).
          this.copyFromUri(uri, this.baseDir + '/data.xp3');
        } else if (lower.endsWith('.zip')) {
          const tmp = cacheDir + '/' + name;
          this.copyFromUri(uri, tmp);
          // Clear a stale data.xp3 so decompress won't hit a conflict
          // (error 900002), then extract to the app root.
          try {
            fs.unlinkSync(this.baseDir + '/data.xp3');
          } catch (e) {
            // none present
          }
          await this.unzipTo(tmp, this.baseDir);
          // If the zip held a root-level data.xp3 it now sits beside
          // savedata at <base>/data.xp3, which the engine reads directly.
          try {
            fs.unlinkSync(tmp);
          } catch (e) {
            // ignore
          }
        } else if (lower.endsWith('.7z') || lower.endsWith('.001') || lower.endsWith('.002')) {
          this.message = '暂不支持 7z 格式，请使用 zip 压缩包或 data.xp3';
          await this.showNotice('暂不支持 7z 格式', '请选择 zip 压缩包或 data.xp3 文件');
          continue;
        } else {
          this.message = '不支持的文件类型：' + name;
          await this.showNotice('不支持的文件类型', '请选择 zip 压缩包或 data.xp3 文件');
          continue;
        }
      } catch (e) {
        this.bootState = 2;
        const detail = JSON.stringify(e);
        this.message = '导入失败：' + name + '\n' + detail + '\nuri: ' + uri;
        await this.showNotice('导入失败', detail);
        return;
      }
      await this.delay(50);
    }
    await this.finishData();
    this.uiEpoch++;
  }

  finishData(): void {
    if (this.dataReady()) {
      // Do not launch automatically: keep the verified state visible and let
      // the user start the native renderer explicitly.
      this.readyForGame = true;
      this.bootState = 2;
      this.progressText = '游戏数据已就绪';
      this.message = '';
      this.uiEpoch++;
      this.log('import/download complete; waiting for explicit game start');
      return;
    }
    this.bootState = 2;
    this.progressValue = 0;
    this.message = '压缩包内容不正确：未找到 data 文件夹或 data.xp3，请重新选择或下载';
    this.showNotice('数据不完整', '请检查压缩包是否包含 data 文件夹或 data.xp3 文件');
  }

  delay(ms: number): Promise<void> {
    return new Promise<void>((resolve) => setTimeout(resolve, ms));
  }

  startGame(): void {
    if (this.bootState === 6) {
      return; // already running
    }
    entryNapi.setExternalDirs(this.baseDir, this.saveDir);
    this.bootState = 6;
    // NOTE: window rotation is deferred until the engine has started
    // (rotateToLandscape() called from onLoad) - rotating the window at the
    // same moment the XComponent is created crashed libace_compatible on the
    // UI thread (clicking "start game" occasionally crashed).
  }

  private rotateToLandscape(): void {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      window.getLastWindow(ctx).then((mainWindow: window.Window) => {
        mainWindow.setPreferredOrientation(window.Orientation.LANDSCAPE);
        mainWindow.setWindowLayoutFullScreen(true);
        // Hide both system bars so the navigation gesture pill is
        // immersed too (game runs fullscreen edge to edge).
        mainWindow.setWindowSystemBarEnable([]);
      }).catch(() => {
        // best effort
      });
    } catch (e) {
      // best effort
    }
  }

  build() {
    Column() {
      if (this.bootState === 0) {
        Text(this.progressText).fontSize(16).fontColor(Color.Gray)
      } else if (this.bootState === 6) {
        Stack() {
          XComponent({
            id: 'gameview',
            type: XComponentType.SURFACE,
            libraryname: 'entry',
            controller: this.xcController
          })
            .onLoad((context: object) => {
              try {
                // Try to hand the engine a surfaceId so it can create the
                // native window directly (the legacy surface callbacks do not
                // fire on this system).
                let sid = '';
                try {
                  sid = this.xcController.getXComponentSurfaceId();
                } catch (e3) {
                  this.log('getXComponentSurfaceId failed: ' + JSON.stringify(e3));
                }
                this.log('onLoad surfaceId=' + sid);
                entryNapi.setSurfaceId(sid);
                const ok: boolean = entryNapi.onLoad(context) as boolean;
                if (ok) {
                  entryNapi.startEngine();
                  // Keep the game (and its BGM) alive in the background.
                  this.startKeepAlive();
                } else {
                  this.log('onLoad failed to attach the XComponent');
                }
              } catch (e) {
                // never let an exception kill the app
                this.log('onLoad exception: ' + JSON.stringify(e));
              }
            })
            // Full-screen height with 16:9 aspect: the video fills the
            // screen top-to-bottom and letterboxes on the sides only.
            .height('100%')
            .aspectRatio(16 / 9)
            // Report the real component size to the engine: the native
            // OnSurfaceChanged callback never fires on this system, so
            // without this the SDL window size stays at the fallback and
            // touch coordinates are scaled wrong.
            .onAreaChange((oldValue: Area, newValue: Area) => {
              try {
                const w = newValue.width as number;
                const h = newValue.height as number;
                if (w > 0 && h > 0) {
                  // Record the real component pixel size. This is used ONLY to
                  // scale touch coordinates from the physical XComponent space
                  // into the game's logical window space (1920x1080). The SDL
                  // window size itself must stay at the game resolution -
                  // resizing it to the physical size made the game crash
                  // ("Scan line 0 is range over (0 to -1)") because layers are
                  // laid out in the 1920x1080 coordinate system.
                  entryNapi.setSurfaceSize(vp2px(w), vp2px(h));
                  this.log('XComponent size: ' + vp2px(w).toFixed(0) + 'x' + vp2px(h).toFixed(0) + ' px');
                }
              } catch (e) {
                this.log('onAreaChange exception: ' + JSON.stringify(e));
              }
            })
            // Video XComponent: the AVPlayer renders the OP/logo movies into
            // THIS surface. The native surface layer ignores zIndex/offset,
            // so the ONLY reliable way to hide it is to unmount the component
            // entirely when idle (the AVPlayer releases it, else its black
            // frame covers the engine forever).
            if (this.videoOnTop) {
            XComponent({
              id: 'videoview',
              type: XComponentType.SURFACE,
              libraryname: 'entry',
              controller: this.xcVideoController
            })
              .onLoad(() => {
                try {
                  let vsid = '';
                  try {
                    vsid = this.xcVideoController.getXComponentSurfaceId();
                  } catch (e3) {
                    this.log('getVideoXComponentSurfaceId failed: ' + JSON.stringify(e3));
                  }
                  this.log('video onLoad surfaceId=' + vsid);
                  entryNapi.setVideoSurfaceId(vsid);
                } catch (e) {
                  this.log('video onLoad exception: ' + JSON.stringify(e));
                }
              })
              .height('100%')
              .aspectRatio(16 / 9)
              .zIndex(3)
            }
            // No .onTouch here: libace_compatible crashes the UI thread while
            // processing touches on the XComponent node itself (before any
            // ArkTS callback runs). Touches are captured by the transparent
            // overlay below.
          // Transparent overlay captures ALL touches so the XComponent node
          // itself never receives them (libace_compatible crashes processing
          // XComponent touches). Forward to the SDL backend via napi.
          Column()
            // Same size as the XComponent so overlay vp coords map directly
            // onto the SDL window coordinate space.
            .height('100%')
            .aspectRatio(16 / 9)
            .backgroundColor('#00000000')
            .zIndex(4)
            .onTouch((event: TouchEvent) => {
              try {
                let type = 0; // SDL_OHOS_TOUCH_DOWN
                if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
                  type = 1; // SDL_OHOS_TOUCH_UP
                } else if (event.type === TouchType.Move) {
                  type = 2; // SDL_OHOS_TOUCH_MOVE
                }
                // Primary finger drives the mouse simulation (single-tap
                // advances text, skips the logo movie).
                const t = event.touches[0];
                if (t) {
                  entryNapi.sendTouch(type, vp2px(t.x), vp2px(t.y));
                }
                // EVERY finger must reach SDL as a FINGER event: the engine
                // maps a two-finger tap onto the right mouse button, which
                // is how the OP movie is skipped and menus are backed out
                // of. The old code only forwarded touches[0], so two-finger
                // gestures never existed on OHOS.
                const changed = event.changedTouches;
                for (let i = 0; i < changed.length; i++) {
                  const f = changed[i];
                  entryNapi.sendFinger(type, f.id, vp2px(f.x), vp2px(f.y));
                }
                this.log('overlay touch: type=' + type + ' fingers=' + event.touches.length +
                  ' changed=' + changed.length);
              } catch (e) {
                this.log('overlay onTouch exception: ' + JSON.stringify(e));
              }
            })
        }
        .width('100%')
        .height('100%')
      } else if (this.bootState === 2) {
        Scroll() {
          Column({ space: 12 }) {
          Text('').height(24)
          Text('缘之空：高清重制').fontSize(26).fontWeight(FontWeight.Bold).fontColor(Color.White)
          Text('需要游戏数据（约 3.6 GB）：可在线下载，或从本地选择 zip 压缩包 / data.xp3 导入').fontSize(13).fontColor(Color.Gray)
          TextInput({ text: this.baseUrl, placeholder: '下载地址（GitHub Release 页面）' })
            .width('86%')
            .fontSize(12)
            .fontColor(Color.White)
            .placeholderColor(Color.Gray)
            .caretColor(Color.White)
            .backgroundColor('#333333')
            .onChange((value: string) => {
              this.baseUrl = value;
            })
          TextInput({ text: this.proxyUrl, placeholder: '加速代理前缀（留空=直连）' })
            .width('86%')
            .fontSize(12)
            .fontColor(Color.White)
            .placeholderColor(Color.Gray)
            .caretColor(Color.White)
            .backgroundColor('#333333')
            .onChange((value: string) => {
              this.proxyUrl = value;
            })
          Row({ space: 8 }) {
            Button('直连').fontSize(12).height(32).backgroundColor('#666666').onClick(() => {
              this.proxyUrl = '';
            })
            Button('gh-proxy').fontSize(12).height(32).backgroundColor('#2E5A88').onClick(() => {
              this.proxyUrl = 'https://gh-proxy.cn/';
            })
            Button('我的代理').fontSize(12).height(32).backgroundColor('#2E5A88').onClick(() => {
              this.proxyUrl = 'https://proxy.craft-hello.top/proxy/';
            })
          }
          if (this.readyForGame) {
            Button('开始游戏（竖屏切换为横屏）').width(240).height(56).fontSize(18).onClick(() => {
              this.startGame();
            })
            Text('数据已就绪，点击开始。若黑屏请截图 debug 日志').fontSize(12).fontColor(Color.Gray).maxLines(2)
          } else {
            Text('留空=直连 GitHub；加速代理前缀会自动拼在原下载链接前').fontSize(11).fontColor(Color.Gray).maxLines(2)
            Button(this.needInit ? '初始化下载目录（在系统下载目录建专属文件夹）' : '已初始化下载目录（可重新确认）')
              .width('88%').height(48).backgroundColor('#2E5A88').onClick(() => {
                this.initPublicDir();
              })
            Button('下载游戏数据').width(220).height(52).onClick(() => {
              this.downloadAll();
            })
            Button('从本地压缩包导入').width(220).height(52).backgroundColor('#3A5BA0').onClick(() => {
              this.importLocal();
            })
            Button('重新检查').width(220).height(44).backgroundColor('#555555').onClick(() => {
              this.prepare();
            })
          }
          Text(this.message).fontSize(12).fontColor(Color.Red).maxLines(4)
          }
        }
        .width('100%')
        .padding({ top: 20, bottom: 40 })
      } else if (this.bootState === 3 || this.bootState === 4) {
        Column({ space: 14 }) {
          // Progress component may not repaint on some system versions;
          // the percentage text is the reliable progress indicator.
          Text(this.progressText).fontSize(16).fontWeight(FontWeight.Medium)
            .fontColor(Color.White).maxLines(4)
            .backgroundColor('#333333').padding(12).borderRadius(8)
            .textAlign(TextAlign.Center)
          Progress({ value: this.progressValue, total: 100 }).width(260).color(Color.White)
        }
      } else {
        Column({ space: 14 }) {
          Text('需要授权').fontSize(18).fontColor(Color.White)
          Text(this.message).fontSize(12).fontColor(Color.Red).maxLines(6)
          Button('重新授权并检查').width(200).height(48).onClick(() => {
            this.prepare();
          })
        }
      }
    }
    .key('boot-' + this.bootState + '-' + this.uiEpoch)
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
    .expandSafeArea([SafeAreaType.SYSTEM])
  }
};
