import entryNapi from 'libentry.so';
import { common, wantAgent, Want, bundleManager } from '@kit.AbilityKit';
import { backgroundTaskManager } from '@kit.BackgroundTasksKit';
import { fileIo as fs, picker } from '@kit.CoreFileKit';
import { http } from '@kit.NetworkKit';
import zlib from '@ohos.zlib';
import { taskpool, util } from '@kit.ArkTS';
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

/** Result of the native XP3 extraction (libentry extractXp3). */
interface Xp3ExtractResult {
  ok: boolean;
  filesDone?: number;
  filesTotal?: number;
  error?: string;
}

/** Structural subset of wantAgent.WantAgentInfo (the SDK does not export
 *  the type itself); fields match the accepted parameter shape. */
interface KeepAliveAgentInfo {
  wants: Want[];
  operationType: number;
  requestCode: number;
  wantAgentFlags: number[];
}

/** data-assets.json root, shipped as rawfile by the CI workflow. */
interface DataManifest {
  schemaVersion: number;
  releaseTag: string;
  baseUrl: string;
  assets: DataAsset[];
}

/**
 * TaskPool worker: decompress a zip on a background thread. The UI thread
 * must not run zlib.decompressFile on a multi-GB archive: the long
 * synchronous call triggers ANR and the app gets killed mid-import.
 */
@Concurrent
function unzipTask(srcZip: string, outDir: string): string {
  try {
    zlib.decompressFile(srcZip, outDir);
    // Report the top-level entries so a truncated decompression is
    // visible in the debug log instead of failing silently.
    let names: string[] = [];
    try {
      names = fs.listFileSync(outDir);
    } catch (e) {
      names = ['<listerr>'];
    }
    return 'ok:' + names.join(',');
  } catch (e) {
    return 'err:' + JSON.stringify(e);
  }
}

/**
 * TaskPool worker: delete a directory tree iteratively (no recursion:
 * @Concurrent functions may only reference imported or local variables).
 * Runs off the UI thread because leftover krkr_* video temp dirs can be
 * hundreds of MB.
 */
@Concurrent
function removeTreeTask(path: string): void {
  const dirs: string[] = [];
  const stack: string[] = [path];
  while (stack.length > 0) {
    const cur = stack.pop();
    if (cur === undefined || cur === '') {
      continue;
    }
    let entries: string[] = [];
    try {
      entries = fs.listFileSync(cur);
    } catch (e) {
      continue;
    }
    for (const name of entries) {
      const child = cur + '/' + name;
      try {
        fs.unlinkSync(child);
      } catch (e) {
        // not a plain file (directory) - descend into it later
        dirs.push(child);
        stack.push(child);
      }
    }
  }
  // Remove deepest directories first: a child path is always longer than
  // its parent, so sorting by path length descending visits children
  // before parents and rmdir (empty-only) succeeds bottom-up.
  dirs.sort((a: string, b: string): number => b.length - a.length);
  for (const d of dirs) {
    try {
      fs.rmdirSync(d);
    } catch (e) {
      // ignore
    }
  }
  try {
    fs.rmdirSync(path);
  } catch (e) {
    // ignore
  }
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
  private extracting: boolean = false;
  private confirming: boolean = false;
  private extractTaskStarted: boolean = false;
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
  // True while startGame waits for the landscape rotation to settle.
  private startingGame: boolean = false;
  // Heartbeat counter for the UI-thread liveness probe (diagnostic).
  private hbTick: number = 0;
  private hbStart: number = 0;

  aboutToAppear(): void {
    this.hbStart = Date.now();
    this.sandboxFiles = (getContext(this) as common.UIAbilityContext).filesDir;
    this.log('appear t=0');
    // Raise the video XComponent above the game XComponent while the AVPlayer
    // plays; poll the native flag (100ms) instead of a NAPI callback thread.
    setInterval(() => {
      try {
        // UI-thread liveness probe: every ~5s log the tick + engine state.
        if (++this.hbTick % 50 === 0) {
          this.log('ui alive t=' + (Date.now() - this.hbStart) + ' boot=' + this.bootState);
        }
        const wasVideo = this.videoOnTop;
        this.videoOnTop = entryNapi.isVideoPlaying();
        if (wasVideo !== this.videoOnTop) {
          this.log('videoOnTop -> ' + this.videoOnTop + ' t=' + (Date.now() - this.hbStart));
        }
        // The in-game exit button finishes the engine thread; the native
        // side reports "running" until the engine has actually started, so
        // a 0 here only ever means the game really quit. Terminate the app
        // (mirrors the Windows exit-button behaviour) instead of leaving a
        // frozen black XComponent on screen.
        if (this.bootState === 6 && !entryNapi.isEngineRunning()) {
          this.log('engine finished; terminating the app');
          this.stopKeepAlive();
          this.setScreenOn(false);
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
    // Resolve the data location synchronously so an app whose game data is
    // already present goes STRAIGHT into the game: no bootstrap background,
    // no setup buttons, no flicker of the setup page on launch.
    this.probeData();
    this.log('probeData done t=' + (Date.now() - this.hbStart));
    if (this.dataReady()) {
      // Readiness backed only by a root data.xp3 must be extracted into a
      // loose data/ tree before the game starts; a ready loose tree goes
      // straight into the game.
      const rootEntries = this.tryList(this.baseDir);
      const hasLoose = this.hasLooseData();
      if (rootEntries.indexOf('data.xp3') >= 0 && !hasLoose) {
        this.log('xp3 present without loose tree -> auto-extract');
        this.extractAndFinalize(this.baseDir + '/data.xp3');
      } else {
        this.readyForGame = true;
        this.log('ready -> confirm/start');
        this.confirmDataVersion();
      }
    } else {
      this.bootState = 2;
      this.progressText = '需要下载或导入游戏数据';
      const xp3 = this.findXp3();
      if (xp3 !== '') {
        this.log('found data.xp3, auto-extracting: ' + xp3);
        this.extractAndFinalize(xp3);
      } else {
        this.prepare();
      }
    }
  }

  // Capture ArkTS page errors so UI-thread crashes do not wedge the page.
  onPageError(err: Error): void {
    try {
      this.log('PAGE_ERROR: ' + JSON.stringify(err));
    } catch (e) {
      hilog.error(0xD003300, 'YosugaOHOS', 'PAGE_ERROR %{public}s', JSON.stringify(err));
    }
  }

  onPageShow(): void {
    // Orientation is fixed to landscape in module.json5; no runtime rotation.
    // Auto-run the data check / public-dir init shortly after the bootstrap
    // page is visible (avoid stalling startup). Never disturb a game that is
    // already starting (black transition screen).
    setTimeout(() => {
      if (this.bootState === 6 || this.bootState === 5 || this.bootState === 7 ||
        this.startingGame || this.confirming) {
        return;
      }
      this.prepare();
    }, 600);
    // Re-acquire the keep-alive task when returning from the background
    // (the system may have cancelled it while the app was hidden).
    if (this.bootState === 6) {
      this.startKeepAlive();
    }
  }

  async prepare(): Promise<void> {
    if (this.bootState === 6 || this.bootState === 5 || this.bootState === 7 ||
      this.startingGame || this.confirming) {
      return; // already running / starting / extracting / asking
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
    hilog.info(0xD003300, 'YosugaOHOS', '%{public}s', msg);
    // Mirror onto the on-screen debug area and append to the sandbox
    // debug.log (hdc-readable) - hilog is not available in release builds.
    const line = Date.now() % 100000 + ': ' + msg;
    this.debugOut = (this.debugOut + '\n' + line).slice(-800);
    try {
      const fd = fs.openSync(this.sandboxFiles + '/debug.log',
        fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.APPEND);
      fs.writeSync(fd.fd, line + '\n');
      fs.closeSync(fd);
    } catch (e) {
      // logging must never break the app
    }
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
      this.log('keepAlive begin t=' + (Date.now() - this.hbStart));
      const ctx = getContext(this) as common.UIAbilityContext;
      const want: Want = {
        bundleName: ctx.abilityInfo.bundleName,
        abilityName: 'EntryAbility'
      };
      const wantAgentInfo: KeepAliveAgentInfo = {
        wants: [want],
        operationType: wantAgent.OperationType.START_ABILITY,
        requestCode: 0,
        wantAgentFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
      };
      const agent = await wantAgent.getWantAgent(wantAgentInfo);
      await backgroundTaskManager.startBackgroundRunning(
        ctx,
        backgroundTaskManager.BackgroundMode.AUDIO_PLAYBACK,
        agent
      );
      this.keepAliveStarted = true;
      this.log('keepAlive started t=' + (Date.now() - this.hbStart));
    } catch (e) {
      // Keep-alive is best effort: the app must never die over it.
      this.log('startBackgroundRunning failed: ' + JSON.stringify(e));
    }
  }

  /** Continuous task for the extraction phase (data transfer mode):
   *  the system exempts the app from resource reclamation while a long
   *  extraction runs. Stops automatically when the extraction ends. */
  private async startExtractTask(): Promise<void> {
    try {
      if (this.extractTaskStarted) {
        return;
      }
      const ctx = getContext(this) as common.UIAbilityContext;
      const want: Want = {
        bundleName: ctx.abilityInfo.bundleName,
        abilityName: 'EntryAbility'
      };
      const wantAgentInfo: KeepAliveAgentInfo = {
        wants: [want],
        operationType: wantAgent.OperationType.START_ABILITY,
        requestCode: 0,
        wantAgentFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
      };
      const agent = await wantAgent.getWantAgent(wantAgentInfo);
      await backgroundTaskManager.startBackgroundRunning(
        ctx,
        backgroundTaskManager.BackgroundMode.DATA_TRANSFER,
        agent
      );
      this.extractTaskStarted = true;
      this.log('extract continuous task started');
    } catch (e) {
      this.log('startExtractTask failed: ' + JSON.stringify(e));
    }
  }

  private async stopExtractTask(): Promise<void> {
    try {
      if (!this.extractTaskStarted) {
        return;
      }
      await backgroundTaskManager.stopBackgroundRunning(
        getContext(this) as common.UIAbilityContext
      );
      this.extractTaskStarted = false;
    } catch (e) {
      // ignore
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

  /** Immersive fullscreen: hide the navigation bar without any
   *  orientation call (window-property IPCs only). */
  private applyImmersive(): void {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      window.getLastWindow(ctx).then((mainWindow: window.Window) => {
        mainWindow.setWindowLayoutFullScreen(true);
        mainWindow.setWindowSystemBarEnable([]);
      }).catch(() => {
        // best effort
      });
    } catch (e) {
      // best effort
    }
  }

  /** Keep the screen on while the game runs (no permission needed). */
  private async setScreenOn(keep: boolean): Promise<void> {
    try {
      this.log('setScreenOn(' + keep + ') begin t=' + (Date.now() - this.hbStart));
      const ctx = getContext(this) as common.UIAbilityContext;
      const win = await window.getLastWindow(ctx);
      await win.setWindowKeepScreenOn(keep);
      this.log('setScreenOn(' + keep + ') done t=' + (Date.now() - this.hbStart));
    } catch (e) {
      this.log('setScreenOn failed: ' + JSON.stringify(e));
    }
  }

  /** Remove leftover random krkr_* video temp dirs from previous runs
   *  (older builds extracted every movie into a fresh random folder).
   *  The fixed krkr_movie_cache folder is skipped: it is the reusable
   *  movie cache that the engine refills on demand. The removal runs on a
   *  TaskPool worker: the dirs can hold hundreds of MB and deleting them
   *  synchronously on the UI thread froze the first launch after an
   *  import (appfreeze). */
  private cleanKrkrTmp(): void {
    const tmpDir = this.baseDir + '/tmp';
    let entries: string[] = [];
    try {
      entries = fs.listFileSync(tmpDir);
    } catch (e) {
      return;
    }
    for (const name of entries) {
      if (name.startsWith('krkr_') && name !== 'krkr_movie_cache') {
        taskpool.execute(removeTreeTask, tmpDir + '/' + name);
      }
    }
  }

  /** Write .nomedia into a directory so the media library skips scanning it. */
  private writeNomedia(dir: string): void {
    try {
      const f = fs.openSync(dir + '/.nomedia',
        fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.writeSync(f.fd, '');
      fs.closeSync(f);
    } catch (e) {
      // best effort
    }
  }

  /**
   * Synchronously resolve baseDir/saveDir/dataDir from the app-owned public
   * Download folder (marker-present) or the sandbox, so a data-ready app can
   * skip the bootstrap UI entirely on launch. Idempotent.
   */
  probeData(): void {
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
    // sandbox so downloads/imports always work.
    this.appDir = DOWNLOAD_ROOT + '/' + APP_FOLDER;
    // Only treat the public folder as usable when the marker we wrote after
    // initPublicDir() is actually listed there. listFileSync on a missing
    // folder throws, so an un-granted platform reliably falls into the
    // sandbox branch.
    let publicOk = false;
    try {
      const entries = fs.listFileSync(this.appDir);
      publicOk = entries.indexOf('.granted') >= 0;
      if (publicOk) {
        fs.mkdirSync(this.appDir + '/savedata', true);
        this.writeNomedia(this.appDir);
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
      this.message = '数据暂存于应用私有目录';
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
  }

  prepareInternal(): void {
    this.log('prepareInternal begin');
    this.probeData();
    const ready = this.dataReady();
    this.readyForGame = ready;
    this.log('dataReady=' + ready + ' dataDir=' + this.dataDir);
    if (ready) {
      // Readiness may come from a data.xp3 archive on the root; that must
      // be extracted into a loose data/ tree before the game can run.
      const rootEntries = this.tryList(this.baseDir);
      const hasLoose = this.hasLooseData();
      if (rootEntries.indexOf('data.xp3') >= 0 && !hasLoose) {
        this.extractAndFinalize(this.baseDir + '/data.xp3');
        return;
      }
      if (this.startingGame || this.bootState === 5) {
        return; // startGame already waits for the rotation; keep the black screen
      }
      this.bootState = 2;
      this.progressText = '游戏数据已就绪';
      this.message = '';
      this.assets = [];
      // Launch as soon as the data is verified, once the page has had a
      // moment to render (no explicit start button).
      setTimeout(() => {
        if (this.bootState === 2 && this.readyForGame && !this.confirming) {
          this.startGame();
        }
      }, 400);
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
      this.writeNomedia(this.appDir);
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
      this.writeNomedia(this.appDir);
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
    // All downloaded zips extract into ONE staging dir on the same
    // volume as the final public data folder (multi-part archives
    // accumulate there); the DATA_TRANSFER task protects the long
    // decompression.
    const unzipTmp = this.baseDir + '/unzip.tmp';
    await taskpool.execute(removeTreeTask, unzipTmp);
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
      await this.unzipTo(tmp, unzipTmp);
      try {
        fs.unlinkSync(tmp);
      } catch (e) {
        // ignore
      }
      doneBytes += asset.size;
      this.progressValue = Math.min(99, Math.floor(doneBytes * 100 / Math.max(1, totalBytes)));
      await this.delay(50);
    }
    this.progressText = '正在准备游戏数据…';
    this.uiEpoch++;
    const ok = await this.installUnzipped(unzipTmp);
    if (!ok) {
      this.bootState = 2;
      this.progressValue = 0;
      this.message = '压缩包内容不正确：未找到 data 文件夹或 data.xp3，请重新选择或下载';
      await this.showNotice('数据不完整', '请检查压缩包是否包含 data 文件夹或 data.xp3 文件');
    }
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
    // Run the decompression on a TaskPool worker: zlib.decompressFile on a
    // multi-GB archive blocks the UI thread long enough to trigger ANR and
    // the system kills the app mid-import. The DATA_TRANSFER continuous
    // task protects the long decompression from resource reclamation.
    await this.startExtractTask();
    try {
      const result = await taskpool.execute(unzipTask, srcZip, outDir) as string;
      this.log('unzip result: ' + result.slice(0, 600));
      if (!result.startsWith('ok:')) {
        throw new Error(result);
      }
    } finally {
      await this.stopExtractTask();
    }
  }

  /** True only when the Windows krkrz-style root contains data.xp3, or
   *  when a complete loose data tree exists in <base>/data/. */
  dataReady(): boolean {
    // Do not use accessSync: this device reports nonexistent paths as present.
    // listFileSync gives us the actual directory entries instead.
    const rootEntries = this.tryList(this.baseDir);
    const dataEntries = this.tryList(this.dataDir);
    const sandboxData = this.tryList(this.sandboxFiles + '/data');
    const hasRootXp3 = rootEntries.indexOf('data.xp3') >= 0;
    const hasStartup = dataEntries.indexOf('startup.tjs') >= 0;
    const hasSystem = dataEntries.indexOf('system') >= 0;
    const hasLooseTree = (hasStartup && hasSystem) ||
      (sandboxData.indexOf('startup.tjs') >= 0 &&
        sandboxData.indexOf('system') >= 0);
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

  /** A loose data tree is ready in the public folder OR in the sandbox
   *  (the extraction writes into the sandbox to keep the media scanner
   *  away; the engine then reads data/ from there). Both startup.tjs and
   *  the system folder must exist so a partial tree never counts. */
  private hasLooseData(): boolean {
    const pub = this.tryList(this.dataDir);
    const sbox = this.tryList(this.sandboxFiles + '/data');
    return (pub.indexOf('startup.tjs') >= 0 && pub.indexOf('system') >= 0) ||
      (sbox.indexOf('startup.tjs') >= 0 && sbox.indexOf('system') >= 0);
  }

  private isDir(path: string): boolean {
    try {
      return fs.statSync(path).isDirectory();
    } catch (e) {
      return false;
    }
  }

  /** After unzipping into the staging dir, install the result:
   *  - "data/" folder  -> becomes the public loose tree
   *  - "data.xp3"      -> handed to the native extractor
   *  - bare tree       -> the staging dir itself becomes the loose tree
   *  Returns true when game data is ready. The staging dir must live on
   *  the same volume as the final data dir so the rename is atomic. */
  private async installUnzipped(unzipTmp: string): Promise<boolean> {
    const items = this.tryList(unzipTmp);
    const swapIn = async (src: string): Promise<void> => {
      if (this.tryList(this.baseDir).indexOf('data') >= 0) {
        await taskpool.execute(removeTreeTask, this.dataDir);
      }
      fs.renameSync(src, this.dataDir);
      try {
        fs.unlinkSync(this.baseDir + '/data.xp3');
      } catch (e) {
        // none present
      }
      this.finishData();
      this.uiEpoch++;
    };
    if (this.isDir(unzipTmp + '/data')) {
      await swapIn(unzipTmp + '/data');
      await taskpool.execute(removeTreeTask, unzipTmp);
      return true;
    } else if (items.indexOf('data.xp3') >= 0) {
      await this.extractAndFinalize(unzipTmp + '/data.xp3');
      await taskpool.execute(removeTreeTask, unzipTmp);
      return true;
    } else if (items.indexOf('startup.tjs') >= 0) {
      await swapIn(unzipTmp);
      return true;
    }
    return false;
  }

  /** Ask the user ONCE per app version whether to re-import the game
   *  package (after an app update the sandbox still holds the previous
   *  data) or keep using the old data. */
  private async confirmDataVersion(): Promise<void> {
    this.confirming = true;
    try {
      const ver = String(this.getVersionCode());
      const markerPath = this.sandboxFiles + '/data_confirm.txt';
      if (this.tryReadFile(markerPath).trim() === ver) {
        this.startGame();
        return;
      }
      this.bootState = 2;
      this.progressText = '游戏数据已就绪';
      this.uiEpoch++;
      const reimport = await this.showConfirm(
        '检测到已有游戏数据',
        '是否重新导入游戏包？' + String.fromCharCode(10) +
          '“重新导入”将删除现有数据并跳转到导入页面；' + String.fromCharCode(10) +
          '“使用旧数据”将直接启动。',
        '重新导入', '使用旧数据');
      if (reimport) {
        this.log('user chose re-import; dropping old data');
        await taskpool.execute(removeTreeTask, this.sandboxFiles + '/data');
        await taskpool.execute(removeTreeTask, this.dataDir);
        this.writeTextFile(markerPath, ver);
        this.readyForGame = false;
        this.bootState = 2;
        this.prepare();
      } else {
        this.writeTextFile(markerPath, ver);
        this.startGame();
      }
    } catch (e) {
      this.log('confirmDataVersion error: ' + this.errText(e));
      this.startGame();
    } finally {
      this.confirming = false;
    }
  }

  private async showConfirm(title: string, msg: string, yesText: string,
    noText: string): Promise<boolean> {
    try {
      const res = await promptAction.showDialog({
        title: title,
        message: msg,
        buttons: [
          { text: yesText, color: '#FF3B30' },
          { text: noText, color: '#007DFF' }
        ]
      });
      return res.index === 0;
    } catch (e) {
      return false;
    }
  }

  private getVersionCode(): number {
    try {
      const info = bundleManager.getBundleInfoForSelfSync(
        bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION);
      return info.versionCode;
    } catch (e) {
      return 0;
    }
  }

  private writeTextFile(path: string, content: string): void {
    try {
      const fd = fs.openSync(path,
        fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.writeSync(fd.fd, content);
      fs.closeSync(fd);
    } catch (e) {
      // ignore
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
  async copyFromUri(uri: string, dest: string): Promise<void> {
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
    let bytes = 0;
    try {
      while (true) {
        const n = fs.readSync(src.fd, buf);
        if (n <= 0) {
          break;
        }
        const chunk = buf.slice(0, n);
        fs.writeSync(dst.fd, chunk);
        bytes += n;
        // Yield to the UI thread periodically: copying a multi-GB zip in
        // one synchronous loop stalls rendering and the system may kill
        // the app (the import crashed after a small part was copied).
        if (bytes >= 32 * 1024 * 1024) {
          bytes = 0;
          await this.delay(0);
        }
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
    this.progressText = '正在导入，请稍等';
    this.progressValue = 0;
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
          // A bare data.xp3 lands on the app root (beside savedata, like
          // Windows krkrz), then gets extracted into a loose data/ tree and
          // deleted. The extraction owns the rest of the flow.
          await this.copyFromUri(uri, this.baseDir + '/data.xp3');
          await this.extractAndFinalize(this.baseDir + '/data.xp3');
          return;
        } else if (lower.endsWith('.zip')) {
          const tmp = cacheDir + '/' + name;
          await this.copyFromUri(uri, tmp);
          // Unzip into a staging dir on the same volume as the final
          // public data folder (atomic rename afterwards). The DATA_TRANSFER
          // continuous task protects this long decompression from resource
          // reclamation.
          const unzipTmp = this.baseDir + '/unzip.tmp';
          await taskpool.execute(removeTreeTask, unzipTmp);
          this.progressText = '正在解压 ' + name;
          await this.unzipTo(tmp, unzipTmp);
          try {
            fs.unlinkSync(tmp);
          } catch (e) {
            // ignore
          }
          const ok = await this.installUnzipped(unzipTmp);
          if (!ok) {
            this.bootState = 2;
            this.progressValue = 0;
            this.message = '压缩包内容不正确：未找到 data 文件夹或 data.xp3，请重新选择或下载';
            await this.showNotice('数据不完整', '请检查压缩包是否包含 data 文件夹或 data.xp3 文件');
          }
          return;
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

  /** First data.xp3 candidate: the app root, then the app-owned public
   *  Download folder. Returns '' when none exists. */
  private findXp3(): string {
    if (this.tryList(this.baseDir).indexOf('data.xp3') >= 0) {
      return this.baseDir + '/data.xp3';
    }
    const dl = DOWNLOAD_ROOT + '/' + APP_FOLDER;
    if (this.tryList(dl).indexOf('data.xp3') >= 0) {
      return dl + '/data.xp3';
    }
    return '';
  }

  /** Extract a data.xp3 archive into <base>/data: unpack to a temp tree
   *  (native worker thread), swap it in with an atomic rename, then delete
   *  the xp3. A crash mid-flow never leaves a half-extracted tree that
   *  looks complete - the temp tree is either renamed in on the next
   *  launch or discarded and re-extracted. */
  async extractAndFinalize(xp3Path: string): Promise<void> {
    if (this.extracting || this.bootState === 6) {
      return;
    }
    this.extracting = true;
    // Extract straight into the public app folder (Download/<bundle>) so
    // the data is visible in the file manager. The DATA_TRANSFER
    // continuous task exempts this long run from resource reclamation,
    // and the resumable extractor survives a kill anyway. When the public
    // folder was never granted, baseDir falls back to the sandbox and the
    // same code path still works.
    const outTmp = this.baseDir + '/data.extract.tmp';
    const outDir = this.baseDir + '/data';
    this.bootState = 7;
    this.progressText = '正在解包 data.xp3…';
    this.progressValue = 0;
    this.message = '';
    this.uiEpoch++;
    this.log('extractAndFinalize begin: ' + xp3Path);
    try {
      // Report where a previous (killed) run stopped so crashes during
      // extraction are diagnosable from debug.log on the next launch.
      const lastProgress = this.tryReadFile(outTmp + '.progress');
      if (lastProgress !== '') {
        this.log('last extract progress: ' + lastProgress.trim());
      }
      // Recover ONLY a fully finished previous run: the worker writes
      // "ok ..." into <outTmp>.status only after every file extracted. A
      // killed run leaves no status (or a stale one) and its PARTIAL tree
      // must be discarded and re-extracted - never renamed into place.
      const lastStatus = this.tryReadFile(outTmp + '.status');
      const hasTmp = this.tryList(this.baseDir).indexOf('data.extract.tmp') >= 0;
      const hasData = this.tryList(outDir).indexOf('startup.tjs') >= 0;
      if (hasTmp && !hasData && lastStatus.startsWith('ok')) {
        try {
          fs.renameSync(outTmp, outDir);
        } catch (e) {
          // fall through and re-extract
        }
      }
      if (this.tryList(outDir).indexOf('startup.tjs') >= 0) {
        this.log('recovered extracted tree; removing archive files');
        try {
          fs.unlinkSync(xp3Path);
        } catch (e) {
          // ignore
        }
        try {
          fs.unlinkSync(DOWNLOAD_ROOT + '/' + APP_FOLDER + '/data.xp3');
        } catch (e) {
          // ignore
        }
        this.finishData();
        return;
      }
      // KEEP any partial temp tree: the extractor skips files whose size
      // already matches, so a killed run resumes from where it stopped.
      this.log('extract step: resuming in ' + outTmp);
      // Ask the system for a DATA_TRANSFER continuous task: it exempts
      // this long extraction from resource reclamation.
      await this.startExtractTask();
      this.log('extract step: calling native extractXp3Start');
      const started = entryNapi.extractXp3Start(xp3Path, outTmp) as boolean;
      this.log('extract step: started=' + started);
      if (!started) {
        this.bootState = 2;
        this.progressText = '解包失败，可重新导入或下载';
        this.message = '解包失败：无法启动解包线程';
        this.uiEpoch++;
        await this.showNotice('解包失败', '无法启动解包线程');
        return;
      }
      // The native worker reports through <outTmp>.status / <outTmp>.progress
      // (polled - the same pattern as the rest of the shell; the OHOS
      // napi_threadsafe_function rejected our create call with
      // napi_invalid_arg, so no NAPI callback is used).
      const statusPath = outTmp + '.status';
      const progressPath = outTmp + '.progress';
      let done = 0;
      let total = 0;
      let pollCount = 0;
      while (true) {
        await this.delay(400);
        pollCount++;
        if (pollCount % 10 === 0) {
          this.log('extract polling alive, done=' + done + '/' + total);
        }
        const st = this.tryReadFile(statusPath);
        if (st !== '') {
          this.log('extract step: status=' + st.trim());
          if (st.startsWith('ok')) {
            break;
          }
          const errMsg = st.startsWith('error ') ? st.substring(6).trim() : st.trim();
          const diagText = this.tryReadFile(outTmp + '.diag');
          this.log('extract diag: ' + (diagText !== '' ? diagText.slice(-2000) : '(empty)'));
          this.bootState = 2;
          this.progressText = '解包失败，可重新导入或下载';
          this.message = '解包失败：' + (errMsg !== '' ? errMsg : '未知错误');
          this.uiEpoch++;
          await this.showNotice('解包失败', errMsg !== '' ? errMsg : '未知错误');
          return;
        }
        const pg = this.tryReadFile(progressPath);
        if (pg !== '') {
          const parts = pg.trim().split(' ');
          if (parts.length >= 2) {
            const pd = Number(parts[0]);
            const pt = Number(parts[1]);
            /* Guard against partially-written progress lines: only accept
             * finite positive values (NaN would crash the Progress node). */
            if (isFinite(pd) && isFinite(pt) && pd >= 0 && pt > 0) {
              done = pd;
              total = pt;
              this.progressValue = Math.floor(done * 100 / total);
              this.progressText = '正在解包 data.xp3：' + done + ' / ' + total + ' 个文件';
            }
          }
        }
      }
      // Swap the extracted tree in, replacing ANY previous data tree
      // (partial or complete) - rename fails when the target exists.
      if (this.tryList(this.baseDir).indexOf('data') >= 0) {
        this.progressText = '正在替换旧数据…';
        this.uiEpoch++;
        await taskpool.execute(removeTreeTask, outDir);
      }
      let renamed = false;
      for (let attempt = 0; attempt < 5; attempt++) {
        try {
          fs.renameSync(outTmp, outDir);
          renamed = true;
          break;
        } catch (eR) {
          this.log('rename attempt ' + attempt + ' failed: ' + this.errText(eR));
          await this.delay(800);
        }
      }
      if (!renamed) {
        throw new Error('rename into place failed');
      }
      this.log('extract done; removing archive files');
      try {
        fs.unlinkSync(outTmp + '.status');
      } catch (e) {
        // ignore
      }
      try {
        fs.unlinkSync(outTmp + '.progress');
      } catch (e) {
        // ignore
      }
      try {
        fs.unlinkSync(xp3Path);
      } catch (e) {
        // ignore
      }
      try {
        fs.unlinkSync(DOWNLOAD_ROOT + '/' + APP_FOLDER + '/data.xp3');
      } catch (e) {
        // ignore
      }
      this.finishData();
    } catch (e) {
      this.bootState = 2;
      this.progressText = '解包失败，可重新导入或下载';
      this.message = '解包失败：' + this.errText(e);
      this.uiEpoch++;
      this.log('extractAndFinalize failed: ' + this.errText(e));
    } finally {
      await this.stopExtractTask();
      this.extracting = false;
    }
  }

  /** Read a small text file; returns '' when missing/unreadable. */
  private tryReadFile(path: string): string {
    try {
      return fs.readTextSync(path);
    } catch (e) {
      return '';
    }
  }

  /** Best-effort human-readable rendering of a caught value (NAPI errors
   *  serialize to "{}" with JSON.stringify). */
  private errText(e: Object): string {
    try {
      const s = JSON.stringify(e);
      if (s !== undefined && s !== '{}') {
        return s;
      }
    } catch (e2) {
      // fall through
    }
    try {
      const anyE = e as Error;
      if (anyE.message !== undefined && anyE.message !== '') {
        return (anyE.name ? anyE.name + ': ' : '') + anyE.message;
      }
    } catch (e3) {
      // fall through
    }
    try {
      return String(e);
    } catch (e4) {
      return 'unknown';
    }
  }

  finishData(): void {
    if (this.dataReady()) {
      // A zip download may have delivered a root-level data.xp3; extract it
      // into a loose data/ tree before starting (and then remove the xp3).
      const rootEntries = this.tryList(this.baseDir);
      const hasLoose = this.hasLooseData();
      if (rootEntries.indexOf('data.xp3') >= 0 && !hasLoose) {
        this.extractAndFinalize(this.baseDir + '/data.xp3');
        return;
      }
      this.readyForGame = true;
      // Record that THIS app version already confirmed the current data,
      // so an app update asks again exactly once.
      this.writeTextFile(this.sandboxFiles + '/data_confirm.txt',
        String(this.getVersionCode()));
      // Go straight to the black transition screen - never flash the setup
      // page after a download/import finished.
      this.bootState = 5;
      this.progressText = '游戏数据已就绪';
      this.message = '';
      this.uiEpoch++;
      this.log('import/download complete; auto-starting the game');
      setTimeout(() => {
        if (this.bootState === 5 && this.readyForGame) {
          this.startGame();
        }
      }, 400);
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
    if (this.bootState === 6 || this.startingGame) {
      return; // already running / already starting
    }
    this.startingGame = true;
    this.log('startGame begin t=' + (Date.now() - this.hbStart));
    this.cleanKrkrTmp();
    // The loose tree lives in the public folder (Download/<bundle>/data);
    // fall back to the sandbox only for data left by older builds.
    let engineBase = this.baseDir;
    if (this.tryList(this.dataDir).indexOf('startup.tjs') < 0 &&
      this.tryList(this.sandboxFiles + '/data').indexOf('startup.tjs') >= 0) {
      engineBase = this.sandboxFiles;
    }
    this.log('engine base dir: ' + engineBase);
    entryNapi.setExternalDirs(engineBase, this.saveDir);
    // Orientation is fixed to landscape in module.json5. Still wait ~1s on
    // a black screen before mounting the XComponent: starting the engine
    // immediately after the page appears crashed libace_compatible.
    this.bootState = 5; // black transition screen
    setTimeout(() => {
      this.log('transition done -> bootState=6 t=' + (Date.now() - this.hbStart));
      this.bootState = 6;
      this.startingGame = false;
    }, 1000);
  }

  private rotateToLandscape(): void {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      window.getLastWindow(ctx).then((mainWindow: window.Window) => {
        this.log('rotateToLandscape got window t=' + (Date.now() - this.hbStart));
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
    Stack({ alignContent: Alignment.Center }) {
      // Title background image shipped inside the HAP (rawfile); only on
      // the bootstrap page - the running game sits on plain black.
      if (this.bootState !== 6 && this.bootState !== 5) {
        Image($rawfile('Background.png'))
          .width('100%')
          .height('100%')
          .objectFit(ImageFit.Cover)
      }
      Column() {
      if (this.bootState === 0) {
        Text(this.progressText).fontSize(16).fontColor(Color.Gray)
      } else if (this.bootState === 5) {
        // Black screen while the window settles in landscape before the
        // engine starts.
        Column() {}
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
                this.log('XComponent onLoad begin t=' + (Date.now() - this.hbStart));
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
                this.log('XComponent onLoad native-attach done');
                const ok: boolean = entryNapi.onLoad(context) as boolean;
                this.log('XComponent onLoad attached=' + ok);
                if (ok) {
                  entryNapi.startEngine();
                  this.log('startEngine called');
                  // Defer ALL window/system IPCs away from the engine
                  // startup: calling them right after an import froze the UI
                  // thread (appfreeze - the system killed the app).
                  setTimeout(() => {
                    this.log('immersive timer fired t=' + (Date.now() - this.hbStart));
                    this.applyImmersive();
                  }, 2000);
                  setTimeout(() => {
                    this.log('deferred keepalive timer fired t=' + (Date.now() - this.hbStart));
                    this.startKeepAlive();
                    this.setScreenOn(true);
                  }, 15000);
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
        Column({ space: 8 }) {
          Text('').height(12)
          Text('缘之空：高清重制').fontSize(26).fontWeight(FontWeight.Bold).fontColor(Color.White)
            .textShadow({ radius: 3, color: '#000000', offsetX: 0, offsetY: 0 })
          // Error messages show right below the title in a slim reserved
          // slot so the whole setup page fits one screen.
          Text(this.message === '' ? ' ' : this.message).fontSize(11).fontColor(Color.Red)
            .maxLines(1).height(18).textAlign(TextAlign.Center)
          Text('需要游戏数据（约 3.6 GB）：可在线下载，或从本地选择 zip 压缩包 / data.xp3 导入').fontSize(13).fontColor(Color.Black)
          TextInput({ text: this.baseUrl, placeholder: '下载地址（留空使用默认值）' })
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
            Button('Craft-Hello Proxy').fontSize(12).height(32).backgroundColor('#2E5A88').onClick(() => {
              this.proxyUrl = 'https://proxy.craft-hello.top/proxy/';
            })
          }
          Text('下载地址留空使用默认值；加速代理前缀会自动拼在原下载链接前').fontSize(11).fontColor(Color.Black).maxLines(2)
          Row({ space: 12 }) {
            Button('下载游戏数据').width(150).height(52).onClick(() => {
              this.downloadAll();
            })
            Button('从本地压缩包导入').width(150).height(52).backgroundColor('#3A5BA0').onClick(() => {
              this.importLocal();
            })
          }
          }
        .width('100%')
        .padding({ top: 20, bottom: 40 })
      } else if (this.bootState === 3 || this.bootState === 4 || this.bootState === 7) {
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
      .width('100%')
      .height('100%')
    }
    .key('boot-' + this.bootState + '-' + this.uiEpoch)
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
    .expandSafeArea([SafeAreaType.SYSTEM])
  }
};
