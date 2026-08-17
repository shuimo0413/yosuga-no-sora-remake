import entryNapi from 'libentry.so';
import { common, abilityAccessCtrl, Permissions, PermissionRequestResult } from '@kit.AbilityKit';
import { fileIo as fs, picker } from '@kit.CoreFileKit';
import { http } from '@kit.NetworkKit';
import zlib from '@ohos.zlib';
import { util } from '@kit.ArkTS';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { promptAction } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

/** data-assets.json entry. */
interface DataAsset {
  name: string;
  size: number;
  sha256: string;
  packs: string[];
  fileCount: number;
}

/** data-assets.json root, shipped as rawfile by the CI workflow. */
interface DataManifest {
  schemaVersion: number;
  releaseTag: string;
  baseUrl: string;
  assets: DataAsset[];
}

const APP_FOLDER = '缘之空高清重制';
const DOWNLOAD_ROOT = '/storage/Users/currentUser/Download';

@Entry
@Component
struct Index {
  // 0 checking, 1 ready, 2 need data, 3 downloading, 4 importing, 5 error, 6 running
  @State bootState: number = 0;
  @State progressText: string = '';
  @State progressValue: number = 0;
  @State baseUrl: string = '';
  @State message: string = '';
  @State uiEpoch: number = 0;
  private sandboxFiles: string = '';
  private baseDir: string = '';
  private saveDir: string = '';
  private appDir: string = '';
  private dataDir: string = '';
  private assets: DataAsset[] = [];
  private manifest: DataManifest | null = null;

  aboutToAppear(): void {
    this.sandboxFiles = (getContext(this) as common.UIAbilityContext).filesDir;
  }

  onPageShow(): void {
    // Re-check on every page show (after the page is fully interactive,
    // otherwise the permission dialog can race the page transition).
    setTimeout(() => {
      this.prepare();
    }, 400);
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
  }

  prepareInternal(): void {
    this.log('prepareInternal begin');
    // NOTE: no permission dialog here. On HarmonyOS 7 (API 26) the system
    // permission dialog is a UIExtension whose close can wipe the host page
    // content (black screen). Permissions are requested on demand when the
    // user taps download/import instead.

    // The game data and savedata should live in the public Download folder;
    // when the platform refuses (denied permission or a changed path model),
    // fall back to the sandbox so the app always boots.
    this.appDir = DOWNLOAD_ROOT + '/' + APP_FOLDER;
    let publicOk = false;
    try {
      fs.mkdirSync(this.appDir, true);
      fs.accessSync(this.appDir);
      publicOk = true;
      this.log('public appDir ok: ' + this.appDir);
    } catch (e) {
      this.log('public appDir unusable: ' + JSON.stringify(e));
    }
    if (publicOk) {
      this.baseDir = this.appDir;
      this.saveDir = this.appDir + '/savedata';
      this.dataDir = this.appDir + '/data';
      try {
        fs.mkdirSync(this.saveDir, true);
      } catch (e) {
        // already exists
      }
      try {
        fs.mkdirSync(this.dataDir, true);
      } catch (e) {
        // already exists
      }
      this.message = '';
    } else {
      this.baseDir = this.sandboxFiles;
      this.saveDir = this.sandboxFiles + '/savedata';
      this.dataDir = this.sandboxFiles + '/data';
      try {
        fs.mkdirSync(this.saveDir, true);
      } catch (e) {
        // already exists
      }
      try {
        fs.mkdirSync(this.dataDir, true);
      } catch (e) {
        // already exists
      }
      this.message = '未获得公共下载目录访问权限，游戏数据将存放在应用私有目录';
      this.log('sandbox fallback: ' + this.baseDir);
    }

    const ready = this.dataReady();
    this.log('dataReady=' + ready + ' dataDir=' + this.dataDir);
    if (ready) {
      this.startGame();
      return;
    }
    this.loadManifest();
    this.log('manifest loaded, assets=' + this.assets.length);
    this.bootState = 2;
    this.progressText = '需要下载或导入游戏数据';
    this.uiEpoch++;
    this.log('bootstrap page shown');
  }

  loadManifest(): void {
    try {
      // Synchronous: an async rawfile read that never settles would leave the
      // UI thread stuck on some system versions (black screen).
      const manager = (getContext(this) as common.UIAbilityContext).resourceManager;
      const bytes = manager.getRawFileContentSync('data-assets.json');
      const text = new util.TextDecoder('utf-8').decodeWithStream(bytes);
      const parsed = JSON.parse(text) as DataManifest;
      this.manifest = parsed;
      this.baseUrl = parsed.baseUrl;
      this.assets = parsed.assets;
    } catch (e) {
      this.message = '读取数据清单失败，请手动设置下载地址';
    }
  }

  /** Request media permissions on demand and switch to the public
   *  Download app folder when granted. Returns true when usable. */
  async ensurePublic(): Promise<boolean> {
    const context = getContext(this) as common.UIAbilityContext;
    const atManager = abilityAccessCtrl.createAtManager();
    const missing: Array<Permissions> = [];
    const perms: Array<Permissions> = ['ohos.permission.READ_MEDIA', 'ohos.permission.WRITE_MEDIA'];
    for (const perm of perms) {
      try {
        const state: number = await atManager.checkAccessToken(context.applicationInfo.accessTokenId, perm);
        if (state !== 0) {
          missing.push(perm);
        }
      } catch (e) {
        missing.push(perm);
      }
    }
    if (missing.length > 0) {
      try {
        const result: PermissionRequestResult = await atManager.requestPermissionsFromUser(context, missing);
        if (!result.authResults.every((code: number) => code === 0)) {
          this.log('permission denied');
          return false;
        }
      } catch (e) {
        this.log('permission request failed: ' + JSON.stringify(e));
        return false;
      }
    }
    try {
      fs.mkdirSync(this.appDir, true);
      fs.accessSync(this.appDir);
    } catch (e) {
      this.log('public dir still unusable: ' + JSON.stringify(e));
      return false;
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
      try {
        fs.mkdirSync(this.dataDir, true);
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
    for (const asset of this.assets) {
      const url = this.baseUrl + '/' + asset.name;
      this.progressText = '正在下载 ' + asset.name;
      const tmp = cacheDir + '/' + asset.name;
      try {
        await this.downloadFile(url, tmp);
        if (asset.sha256 !== '' && !(await this.verifySha256(tmp, asset.sha256))) {
          throw new Error('SHA-256 校验失败，请重试');
        }
      } catch (e) {
        this.bootState = 5;
        this.message = '下载失败：' + asset.name + '（' + JSON.stringify(e) + '）';
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

  async downloadFile(url: string, dest: string): Promise<void> {
    // Range-chunked download: @ohos.net.http has no streaming end event on
    // this SDK, and buffering multi-GiB files in memory is not an option.
    const file = fs.openSync(dest, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    const chunkSize = 8 * 1024 * 1024;
    let offset = 0;
    try {
      while (true) {
        const end = offset + chunkSize - 1;
        const httpRequest = http.createHttp();
        try {
          const response: http.HttpResponse = await httpRequest.request(url, {
            method: http.RequestMethod.GET,
            header: { 'Range': 'bytes=' + offset + '-' + end },
            expectDataType: http.HttpDataType.ARRAY_BUFFER,
            connectTimeout: 60000,
            readTimeout: 180000
          });
          if (response.responseCode === 416) {
            break;
          }
          if (response.responseCode !== 206 && response.responseCode !== 200) {
            throw new Error('HTTP ' + response.responseCode);
          }
          const data = response.result as ArrayBuffer;
          const length = data.byteLength;
          if (length === 0) {
            break;
          }
          fs.writeSync(file.fd, data);
          offset += length;
          if (length < chunkSize) {
            break;
          }
        } finally {
          httpRequest.destroy();
        }
      }
    } finally {
      fs.closeSync(file);
    }
  }

  async unzipTo(srcZip: string, outDir: string): Promise<void> {
    await zlib.decompressFile(srcZip, outDir);
  }

  /** True when the data folder holds a complete data tree or a data.xp3. */
  dataReady(): boolean {
    // Fully synchronous: async probes that never settle on some system
    // versions would freeze the UI thread (black screen).
    // A complete tree has startup.tjs plus the system scripts; a bare
    // startup.tjs (e.g. leftover from an old mini test build) is not enough.
    try {
      const entries = fs.listFileSync(this.dataDir);
      this.log('dataDir entries: ' + JSON.stringify(entries).slice(0, 300));
      const hasStartup = entries.indexOf('startup.tjs') >= 0;
      const hasSystem = entries.indexOf('system') >= 0;
      if (hasStartup && hasSystem) {
        return true;
      }
    } catch (e) {
      this.log('dataDir list failed: ' + JSON.stringify(e));
    }
    try {
      fs.accessSync(this.dataDir + '/data.xp3');
      return true;
    } catch (e) {
      return false;
    }
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
      this.bootState = 2;
      this.message = '未选择文件';
      return;
    }
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
          // A bare data.xp3 goes straight into the data folder.
          fs.mkdirSync(this.dataDir, true);
          fs.copyFileSync(uri, this.dataDir + '/data.xp3');
        } else if (lower.endsWith('.zip')) {
          const tmp = cacheDir + '/' + name;
          fs.copyFileSync(uri, tmp);
          await this.unzipTo(tmp, this.baseDir);
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
        this.message = '导入失败：' + name + '（' + JSON.stringify(e) + '）';
        await this.showNotice('导入失败', JSON.stringify(e));
        return;
      }
      await this.delay(50);
    }
    await this.finishData();
    this.uiEpoch++;
  }

  finishData(): void {
    if (this.dataReady()) {
      // Data verified: jump straight into the game. Next launch skips this
      // screen because onPageShow finds the data ready.
      this.startGame();
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
  }

  build() {
    Column() {
      if (this.bootState === 0) {
        Text(this.progressText).fontSize(16).fontColor(Color.Gray)
      } else if (this.bootState === 6) {
        XComponent({
          id: 'gameview',
          type: XComponentType.SURFACE,
          libraryname: 'entry'
        })
          .onLoad((context: object) => {
            try {
              const ok: boolean = entryNapi.onLoad(context) as boolean;
              if (ok) {
                entryNapi.startEngine();
              } else {
                this.log('onLoad failed to attach the XComponent');
              }
            } catch (e) {
              // never let an exception kill the app
              this.log('onLoad exception: ' + JSON.stringify(e));
            }
          })
          .width('100%')
          .height('100%')
      } else if (this.bootState === 2) {
        Column({ space: 14 }) {
          Text('缘之空：高清重制').fontSize(26).fontWeight(FontWeight.Bold).fontColor(Color.White)
          Text('需要游戏数据（约 3.6 GB）：可在线下载，或从本地选择 zip 压缩包 / data.xp3 导入').fontSize(13).fontColor(Color.Gray)
          TextInput({ text: this.baseUrl, placeholder: '下载地址（GitHub Release 页面）' })
            .width('86%')
            .onChange((value: string) => {
              this.baseUrl = value;
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
          Text(this.message).fontSize(12).fontColor(Color.Red).maxLines(4)
        }
        .width('100%')
        .justifyContent(FlexAlign.Center)
      } else if (this.bootState === 3 || this.bootState === 4) {
        Column({ space: 14 }) {
          Progress({ value: this.progressValue, total: 100 }).width(260).color(Color.White)
          Text(this.progressText).fontSize(13).fontColor(Color.Gray)
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
    .backgroundColor(Color.Black)
  }
};
