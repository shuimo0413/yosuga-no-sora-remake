import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import entryNapi from 'libentry.so';

const TAG: string = 'YosugaOHOS';
const DOMAIN: number = 0x0000;

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'EntryAbility onCreate');
    // Give the native side the ability context (rawfile access) and the
    // sandbox files directory before the game page appears.
    entryNapi.initResourceManager(this.context, this.context.filesDir);
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'EntryAbility onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'EntryAbility onWindowStageCreate');
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'Failed to load content: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, TAG, '%{public}s', 'Succeeded in loading content');
    });
    windowStage.getMainWindow().then((mainWindow: window.Window) => {
      // The bootstrap page is NOT fullscreen so the status bar stays visible
      // (content would otherwise be hidden under the notification bar). The
      // ability itself is declared landscape in module.json5, so no runtime
      // rotation is needed (runtime rotation froze the UI thread on first
      // launches with the screen locked).
      mainWindow.setWindowLayoutFullScreen(false);
    }).catch((err: Error) => {
      hilog.error(DOMAIN, TAG, 'Failed to configure main window: %{public}s', JSON.stringify(err));
    });
  }

  onWindowStageDestroy(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'EntryAbility onWindowStageDestroy');
  }

  onForeground(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'EntryAbility onForeground');
  }

  onBackground(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'EntryAbility onBackground');
  }
}
