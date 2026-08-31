/* SPDX-License-Identifier: MIT */
/*
 * iOS bootstrap page, mirroring the OpenHarmony shell page:
 * - fixed 1920x1080 Background.png with uniform scale-to-fit
 * - transparent hit targets over the artwork's download/proxy labels
 * - optional custom URL/proxy dialog for local builds and mirrors
 * - download / import actions with progress display
 * - screen kept awake while downloading/importing/extracting
 * - status bar hidden (Info.plist), home indicator auto-hidden
 */

#import <UIKit/UIKit.h>
#import <CommonCrypto/CommonDigest.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <string>
#include <cstring>
#include <cstdio>

extern "C" {
#include "IOSBootstrap.h"
#include "zip_extract.h"
#include "xp3_extract.h"
}

/* ------------------------------------------------------------------ */
/* Data root helpers                                                   */
/* ------------------------------------------------------------------ */

static std::string g_ios_data_root;

const char *krkrsdl2_ios_data_root(void)
{
    /* The engine resolves ./data/* through this function from several
     * threads (image loader, storage lookups) during startup; a racy
     * first initialization of the std::string cache would corrupt memory.
     * dispatch_once makes the lazy init thread-safe. */
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSArray *paths = NSSearchPathForDirectoriesInDomains(
            NSDocumentDirectory, NSUserDomainMask, YES);
        NSString *documents = paths.firstObject;
        if (documents && documents.length > 0)
        {
            NSString *folder = NSBundle.mainBundle.bundleIdentifier;
            if (folder.length == 0)
                folder = @"YosugaSoraHD";
            NSString *root = [documents stringByAppendingPathComponent:folder];
            [[NSFileManager defaultManager] createDirectoryAtPath:root
                                      withIntermediateDirectories:YES
                                                       attributes:nil
                                                            error:nil];
            const char *utf8 = root.fileSystemRepresentation;
            if (utf8)
                g_ios_data_root = utf8;
        }
    });
    return g_ios_data_root.empty() ? NULL : g_ios_data_root.c_str();
}

static NSString *DataRootPath(void)
{
    const char *root = krkrsdl2_ios_data_root();
    if (!root || !root[0])
        return nil;
    return [NSString stringWithUTF8String:root];
}

static NSString *DataDirPath(void)
{
    return [DataRootPath() stringByAppendingPathComponent:@"data"];
}

static NSString *CacheDirPath(void)
{
    return [DataRootPath() stringByAppendingPathComponent:@"cache"];
}

static NSString *StagingPath(void)
{
    return [DataRootPath() stringByAppendingPathComponent:@"unzip.tmp"];
}

static BOOL GameDataReady(void)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *startup = [DataDirPath() stringByAppendingPathComponent:@"startup.tjs"];
    NSString *marker = [DataRootPath() stringByAppendingPathComponent:@".complete"];
    return [fm fileExistsAtPath:startup] && [fm fileExistsAtPath:marker];
}

/* Append a diagnostic line to Documents/<bundle>/bootstrap.log so a crash
 * can be located after the fact (the file is reachable via the Files app). */
static void IosLog(NSString *message)
{
    @autoreleasepool {
        const char *root = krkrsdl2_ios_data_root();
        if (!root || !root[0]) return;
        NSString *path = [[NSString stringWithUTF8String:root]
            stringByAppendingPathComponent:@"bootstrap.log"];
        NSString *line = [NSString stringWithFormat:@"%@ %@\n",
            NSDate.date, message];
        NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:path];
        if (h)
        {
            [h seekToEndOfFile];
            [h writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
            [h closeFile];
        }
        else
        {
            [line writeToFile:path atomically:NO
                encoding:NSUTF8StringEncoding error:nil];
        }
    }
}

static void RemoveTree(NSString *path)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:path])
        [fm removeItemAtPath:path error:nil];
}

static void EnsureDir(NSString *path)
{
    [[NSFileManager defaultManager] createDirectoryAtPath:path
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
}

/* Merge src into dst recursively: directories descend, files replace.
 * This matters for the multi-volume data zips: every volume carries its
 * own copy of the data/<subdir>/ tree with DIFFERENT files, and replacing
 * a whole subdirectory wiped the files extracted from the previous volume
 * (the engine then crashed on startup with missing files). */
static BOOL MergeTree(NSString *src, NSString *dst, NSString **errOut)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    BOOL isDir = NO;
    if (![fm fileExistsAtPath:src isDirectory:&isDir])
        return YES;
    if (!isDir)
    {
        RemoveTree(dst);
        NSError *err = nil;
        if (![fm moveItemAtPath:src toPath:dst error:&err])
        {
            if (errOut) *errOut = err.localizedDescription;
            return NO;
        }
        return YES;
    }
    EnsureDir(dst);
    NSArray *items = [fm contentsOfDirectoryAtPath:src error:nil];
    for (NSString *item in items)
    {
        if (!MergeTree([src stringByAppendingPathComponent:item],
            [dst stringByAppendingPathComponent:item], errOut))
            return NO;
    }
    return YES;
}

static void MarkDataComplete(void)
{
    NSString *marker = [DataRootPath() stringByAppendingPathComponent:@".complete"];
    [@"ok" writeToFile:marker atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

static void ClearDataComplete(void)
{
    RemoveTree([DataRootPath() stringByAppendingPathComponent:@".complete"]);
}

/* Merge the staging tree into <root>/data: entries under staging/data are
 * used when present (the CI packer prefixes everything with data/). The
 * prefix check must be structural: startup.tjs lives only in the FIRST
 * volume, and a per-file check made the later volumes merge their whole
 * data/ folder into data/data/ (11 top-level folders missing). */
static BOOL MergeIntoDataDir(NSString *staging, NSString **errOut)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *dataDir = DataDirPath();
    NSString *src = staging;
    NSString *prefixed = [staging stringByAppendingPathComponent:@"data"];
    BOOL prefixedIsDir = NO;
    if ([fm fileExistsAtPath:prefixed isDirectory:&prefixedIsDir] && prefixedIsDir)
        src = prefixed;
    EnsureDir(dataDir);
    NSArray *items = [fm contentsOfDirectoryAtPath:src error:nil];
    if (!items)
    {
        if (errOut) *errOut = @"解压结果为空";
        return NO;
    }
    for (NSString *item in items)
    {
        if (!MergeTree([src stringByAppendingPathComponent:item],
            [dataDir stringByAppendingPathComponent:item], errOut))
            return NO;
    }
    return YES;
}

/* ------------------------------------------------------------------ */
/* SHA-256 (streaming, matches the packer's manifest hashes)           */
/* ------------------------------------------------------------------ */

static BOOL SHA256OfFile(NSString *path, unsigned char out[CC_SHA256_DIGEST_LENGTH],
    NSString **errOut)
{
    FILE *f = fopen(path.fileSystemRepresentation, "rb");
    if (!f)
    {
        if (errOut) *errOut = @"无法打开文件";
        return NO;
    }
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    /* Heap buffer: this runs on a GCD worker thread whose stack is only
     * ~512 KB, and the previous 1 MB on-stack buffer tripped the stack
     * guard ("Thread stack size exceeded", SIGBUS). */
    unsigned char *buf = (unsigned char *)malloc(256 * 1024);
    if (!buf)
    {
        fclose(f);
        if (errOut) *errOut = @"内存不足";
        return NO;
    }
    size_t n = 0;
    while ((n = fread(buf, 1, 256 * 1024, f)) > 0)
        CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
    free(buf);
    fclose(f);
    CC_SHA256_Final(out, &ctx);
    return YES;
}

static NSString *HexString(const unsigned char *bytes, size_t len)
{
    NSMutableString *s = [NSMutableString stringWithCapacity:len * 2];
    for (size_t i = 0; i < len; i++)
        [s appendFormat:@"%02x", bytes[i]];
    return s;
}

/* ------------------------------------------------------------------ */
/* Bootstrap view controller                                           */
/* ------------------------------------------------------------------ */

@interface TVPIOSBootstrapVC : UIViewController

@property (nonatomic) BOOL finished;
@property (nonatomic) int result; /* 1 = data ready */

@end

@interface TVPIOSBootstrapVC () <UIDocumentPickerDelegate, NSURLSessionDownloadDelegate, UITextFieldDelegate>
@property (nonatomic, copy) NSArray *assetList;
@property (nonatomic) NSUInteger assetIndex;
@property (nonatomic) long long doneBytes;
@property (nonatomic) long long totalBytes;
/* Whole-download start instant for the average speed display. */
@property (nonatomic) CFAbsoluteTime downloadStart;
@property (nonatomic, strong) NSURLSession *activeSession;
@property (nonatomic, strong) NSURLSessionDownloadTask *activeTask;
/* 6-way concurrent Range download state (8MB chunks, like the Android
 * build). chunkPlans holds one dict per in-flight data task; the shared
 * NSFileHandle does positional writes so order never matters. */
@property (nonatomic, strong) NSFileHandle *chunkFile;
@property (nonatomic, strong) NSMutableArray<NSDictionary *> *chunkPlans;
@property (nonatomic, assign) long long chunkReceivedTotal;
@property (nonatomic, assign) NSInteger chunksRemaining;
@property (nonatomic, assign) BOOL chunkFailed;
@property (nonatomic, copy) NSDictionary *activeTaskState;
@end

@implementation TVPIOSBootstrapVC
{
    UIImageView *_background;
    UIImageView *_directArtwork;
    UIImageView *_ghProxyArtwork;
    UIImageView *_craftProxyArtwork;
    UIImageView *_downloadArtwork;
    UIImageView *_importArtwork;
    UIImageView *_progressTrack;
    UILabel *_messageLabel;
    UITextField *_urlField;
    UITextField *_proxyField;
    UIButton *_directButton;
    UIButton *_ghProxyButton;
    UIButton *_craftProxyButton;
    UIButton *_downloadButton;
    UIButton *_importButton;
    UILabel *_progressLabel;
    UIView *_progressFill;
    UIView *_container;
    BOOL _busy;
    BOOL _importPickerOpen;
    /* Pointer-hover highlight (mouse / trackpad), mirroring the touch
     * pressed state. */
    NSInteger _hoverAction;
    NSInteger _hoverProxy;
    /* Background execution time while a transfer runs (see setBusy:). */
    UIBackgroundTaskIdentifier _bgTask;
    NSInteger _selectedProxy;
    NSInteger _activeAction;
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (BOOL)shouldAutorotate { return NO; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    return UIInterfaceOrientationMaskLandscape;
}

- (NSString *)defaultBaseUrl
{
    /* Build-time injected default-manifest.json (same shape as OHOS). */
    NSString *path = [[NSBundle mainBundle] pathForResource:@"default-manifest"
                                                     ofType:@"json"];
    if (path)
    {
        NSData *data = [NSData dataWithContentsOfFile:path];
        if (data)
        {
            NSDictionary *json = [NSJSONSerialization JSONObjectWithData:data
                                options:0 error:nil];
            NSString *baseUrl = json[@"baseUrl"];
            if ([baseUrl isKindOfClass:NSString.class] && baseUrl.length > 0)
                return baseUrl;
        }
    }
    return @"https://github.com/shuimo0413/yosuga-no-sora-remake/releases/latest/download/";
}

- (NSString *)effectiveBaseUrl
{
    NSString *text = [_urlField.text stringByTrimmingCharactersInSet:
        NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSString *base = text.length > 0 ? text : [self defaultBaseUrl];
    return [base hasSuffix:@"/"] ? base : [base stringByAppendingString:@"/"];
}

- (NSString *)proxyPrefix
{
    NSString *text = [_proxyField.text stringByTrimmingCharactersInSet:
        NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return text ?: @"";
}

- (UIColor *)colorFromHex:(NSUInteger)hex
{
    return [UIColor colorWithRed:((hex >> 16) & 0xFF) / 255.0
                           green:((hex >> 8) & 0xFF) / 255.0
                            blue:(hex & 0xFF) / 255.0
                           alpha:1.0];
}

- (UITextField *)makeField:(NSString *)placeholder
{
    UITextField *f = [[UITextField alloc] initWithFrame:CGRectZero];
    f.placeholder = placeholder;
    /* Bright placeholder on a darker field: the default gray placeholder
     * was hard to tell apart from the #333333 field background. */
    f.attributedPlaceholder = [[NSAttributedString alloc] initWithString:placeholder
        attributes:@{NSForegroundColorAttributeName: [self colorFromHex:0xC8C8C8]}];
    f.font = [UIFont systemFontOfSize:13];
    f.textColor = UIColor.whiteColor;
    f.backgroundColor = [self colorFromHex:0x1E1E1E];
    f.layer.cornerRadius = 6;
    f.clipsToBounds = YES;
    f.autocapitalizationType = UITextAutocapitalizationTypeNone;
    f.autocorrectionType = UITextAutocorrectionTypeNo;
    f.keyboardType = UIKeyboardTypeURL;
    f.returnKeyType = UIReturnKeyDone;
    f.delegate = (id<UITextFieldDelegate>)self;
    f.leftView = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 10, 1)];
    f.leftViewMode = UITextFieldViewModeAlways;
    return f;
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField
{
    [textField resignFirstResponder];
    return YES;
}

- (UIButton *)makeOverlayButton:(NSString *)accessibilityLabel
{
    UIButton *button = [UIButton buttonWithType:UIButtonTypeCustom];
    button.backgroundColor = UIColor.clearColor;
    button.accessibilityLabel = accessibilityLabel;
    return button;
}

- (UIImageView *)makeAssetView:(NSString *)name frame:(CGRect)frame
{
    UIImage *image = [self assetImage:name];
    UIImageView *view = [[UIImageView alloc] initWithImage:image];
    view.frame = frame;
    view.contentMode = UIViewContentModeScaleToFill;
    view.userInteractionEnabled = NO;
    view.accessibilityElementsHidden = YES;
    return view;
}

- (UIImage *)assetImage:(NSString *)name
{
    NSString *path = [[NSBundle mainBundle] pathForResource:name ofType:@"png"];
    return path ? [UIImage imageWithContentsOfFile:path] : nil;
}

- (void)setArtwork:(UIImageView *)view name:(NSString *)name
{
    view.image = [self assetImage:name];
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    _bgTask = UIBackgroundTaskInvalid;
    self.view.backgroundColor = UIColor.blackColor;

    /* The supplied artwork and every hit target share one 1920x1080
     * coordinate system. viewDidLayoutSubviews only applies a uniform
     * scale, so other resolutions add letterbox space instead of moving or
     * stretching individual controls. */
    UIImage *bg = [UIImage imageWithContentsOfFile:
        [[NSBundle mainBundle] pathForResource:@"Background" ofType:@"png"]];
    _container = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 1920, 1080)];
    _container.backgroundColor = UIColor.blackColor;
    [self.view addSubview:_container];

    _background = [[UIImageView alloc] initWithImage:bg];
    _background.contentMode = UIViewContentModeScaleToFill;
    _background.frame = _container.bounds;
    [_container addSubview:_background];

    // Keep the supplied labels as images instead of rebuilding them with
    // system fonts. These views use the same fixed canvas as the background;
    // transparent buttons below provide the interaction layer.
    _directArtwork = [self makeAssetView:@"github_direct"
                                   frame:CGRectMake(200, 430, 356, 123)];
    _ghProxyArtwork = [self makeAssetView:@"gh_proxy_label"
                                    frame:CGRectMake(600, 440, 338, 105)];
    _craftProxyArtwork = [self makeAssetView:@"craft_hello_label"
                                       frame:CGRectMake(1000, 440, 673, 105)];
    _progressTrack = [self makeAssetView:@"progress_track"
                                   frame:CGRectMake(210, 690, 1215, 26)];
    _downloadArtwork = [self makeAssetView:@"download_label"
                                     frame:CGRectMake(1270, 800, 136, 57)];
    _importArtwork = [self makeAssetView:@"import_label"
                                   frame:CGRectMake(1470, 800, 201, 57)];
    _progressTrack.hidden = YES;
    [_container addSubview:_directArtwork];
    [_container addSubview:_ghProxyArtwork];
    [_container addSubview:_craftProxyArtwork];
    [_container addSubview:_progressTrack];
    [_container addSubview:_downloadArtwork];
    [_container addSubview:_importArtwork];

    _progressFill = [[UIView alloc] initWithFrame:CGRectMake(214, 694, 0, 18)];
    _progressFill.backgroundColor = [self colorFromHex:0x1783FF];
    _progressFill.layer.cornerRadius = 9;
    _progressFill.hidden = YES;
    [_container addSubview:_progressFill];

    _messageLabel = [[UILabel alloc] initWithFrame:CGRectMake(200, 580, 1520, 100)];
    _messageLabel.font = [UIFont systemFontOfSize:22];
    _messageLabel.textColor = UIColor.redColor;
    _messageLabel.textAlignment = NSTextAlignmentCenter;
    _messageLabel.numberOfLines = 2;
    _messageLabel.text = @" ";
    [_container addSubview:_messageLabel];

    // Keep custom URL/proxy support for local builds, but move it into an
    // optional dialog so the normal bootstrap matches the reference artwork.
    _urlField = [self makeField:@"下载地址（留空使用默认值）"];
    _proxyField = [self makeField:@"加速代理前缀（留空=直连）"];

    _directButton = [self makeOverlayButton:@"GitHub直链"];
    _directButton.tag = 1;
    _directButton.frame = CGRectMake(20, 425, 550, 145);
    _ghProxyButton = [self makeOverlayButton:@"GH-PROXY"];
    _ghProxyButton.tag = 2;
    _ghProxyButton.frame = CGRectMake(570, 425, 410, 145);
    _craftProxyButton = [self makeOverlayButton:@"CRAFT-HELLO PROXY"];
    _craftProxyButton.tag = 3;
    [self attachHover:_directButton];
    [self attachHover:_ghProxyButton];
    [self attachHover:_craftProxyButton];
    _craftProxyButton.frame = CGRectMake(980, 425, 740, 145);
    for (UIButton *button in @[_directButton, _ghProxyButton, _craftProxyButton])
    {
        [button addTarget:self action:@selector(onProxyTapped:)
            forControlEvents:UIControlEventTouchUpInside];
        [_container addSubview:button];
    }

    _downloadButton = [self makeOverlayButton:@"开始下载；长按设置下载地址和代理"];
    _downloadButton.tag = 1;
    [_downloadButton addTarget:self action:@selector(onActionTouchDown:)
              forControlEvents:UIControlEventTouchDown];
    [_downloadButton addTarget:self action:@selector(onActionTouchCancelled:)
              forControlEvents:UIControlEventTouchCancel | UIControlEventTouchDragExit |
                               UIControlEventTouchUpOutside];
    [_downloadButton addTarget:self action:@selector(onDownload)
              forControlEvents:UIControlEventTouchUpInside];
    [self attachHover:_downloadButton];
    _downloadButton.frame = CGRectMake(1240, 755, 300, 135);
    [_container addSubview:_downloadButton];
    UILongPressGestureRecognizer *settingsGesture = [[UILongPressGestureRecognizer alloc]
        initWithTarget:self action:@selector(showDownloadSettings:)];
    [_downloadButton addGestureRecognizer:settingsGesture];

    _importButton = [self makeOverlayButton:@"导入本地文件"];
    _importButton.tag = 2;
    [_importButton addTarget:self action:@selector(onActionTouchDown:)
            forControlEvents:UIControlEventTouchDown];
    [_importButton addTarget:self action:@selector(onActionTouchCancelled:)
            forControlEvents:UIControlEventTouchCancel | UIControlEventTouchDragExit |
                             UIControlEventTouchUpOutside];
    [_importButton addTarget:self action:@selector(onImport)
            forControlEvents:UIControlEventTouchUpInside];
    [self attachHover:_importButton];
    _importButton.frame = CGRectMake(1450, 755, 430, 135);
    [_container addSubview:_importButton];

    _progressLabel = [[UILabel alloc] initWithFrame:CGRectMake(235, 710, 1450, 100)];
    _progressLabel.font = [UIFont systemFontOfSize:28];
    _progressLabel.textColor = UIColor.whiteColor;
    _progressLabel.textAlignment = NSTextAlignmentCenter;
    _progressLabel.numberOfLines = 3;
    _progressLabel.hidden = YES;
    [_container addSubview:_progressLabel];

    _selectedProxy = 1;
    [self updateProxyArtwork];

}

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    CGSize bounds = self.view.bounds.size;
    CGFloat scale = MIN(bounds.width / 1920.0, bounds.height / 1080.0);
    _container.bounds = CGRectMake(0, 0, 1920, 1080);
    _container.center = CGPointMake(CGRectGetMidX(self.view.bounds),
                                    CGRectGetMidY(self.view.bounds));
    _container.transform = CGAffineTransformMakeScale(scale, scale);
}

- (void)showDownloadSettings:(UILongPressGestureRecognizer *)gesture
{
    if (gesture.state != UIGestureRecognizerStateBegan)
        return;
    UIAlertController *alert = [UIAlertController
        alertControllerWithTitle:@"下载设置"
                         message:@"下载地址留空使用默认值；代理前缀会拼在原链接前"
                  preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
        field.placeholder = self->_urlField.placeholder;
        field.text = self->_urlField.text;
        field.keyboardType = UIKeyboardTypeURL;
    }];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
        field.placeholder = self->_proxyField.placeholder;
        field.text = self->_proxyField.text;
        field.keyboardType = UIKeyboardTypeURL;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消"
                                               style:UIAlertActionStyleCancel
                                             handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"确定"
                                               style:UIAlertActionStyleDefault
                                             handler:^(UIAlertAction *action) {
        self->_urlField.text = alert.textFields[0].text;
        self->_proxyField.text = alert.textFields[1].text;
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

/* ---- stateful artwork buttons ---- */

- (UIImageView *)proxyArtworkForTag:(NSInteger)tag
{
    if (tag == 1) return _directArtwork;
    if (tag == 2) return _ghProxyArtwork;
    return _craftProxyArtwork;
}

- (NSString *)proxyArtworkNameForTag:(NSInteger)tag suffix:(NSString *)suffix
{
    NSString *base = tag == 1 ? @"github_direct"
        : (tag == 2 ? @"gh_proxy_label" : @"craft_hello_label");
    return suffix.length > 0 ? [base stringByAppendingString:suffix] : base;
}

- (void)onProxyTapped:(UIButton *)button
{
    /* A download source is mandatory; tapping the current source is a no-op. */
    _selectedProxy = button.tag;
    if (_selectedProxy == 2)
        _proxyField.text = @"https://gh-proxy.cn/";
    else if (_selectedProxy == 3)
        _proxyField.text = @"https://proxy.craft-hello.top/proxy/";
    else
        _proxyField.text = @"";
    [self updateProxyArtwork];
}

/* Mouse / trackpad hover (iPad pointer): show the active artwork while the
 * pointer is over a button, restore when it leaves. UIHoverGestureRecognizer
 * is iOS 13+; on older systems this simply does nothing. */
- (void)attachHover:(UIButton *)button
{
    if (@available(iOS 13.0, *))
    {
        UIHoverGestureRecognizer *hover = [[UIHoverGestureRecognizer alloc]
            initWithTarget:self action:@selector(onPointerHover:)];
        [button addGestureRecognizer:hover];
    }
}

- (void)onPointerHover:(UIHoverGestureRecognizer *)recognizer
{
    UIButton *button = (UIButton *)recognizer.view;
    if (![button isKindOfClass:[UIButton class]])
        return;
    BOOL over = recognizer.state == UIGestureRecognizerStateBegan
             || recognizer.state == UIGestureRecognizerStateChanged;
    BOOL isProxy = button == _directButton || button == _ghProxyButton
                || button == _craftProxyButton;
    if (isProxy)
    {
        _hoverProxy = over ? button.tag : (_hoverProxy == button.tag ? 0 : _hoverProxy);
        [self updateProxyArtwork];
    }
    else
    {
        _hoverAction = over ? button.tag : (_hoverAction == button.tag ? 0 : _hoverAction);
        [self updateActionArtwork];
    }
}

- (void)updateProxyArtwork
{
    for (NSInteger tag = 1; tag <= 3; ++tag)
    {
        NSString *suffix = (_selectedProxy == tag || _hoverProxy == tag)
            ? @"_selected" : @"";
        [self setArtwork:[self proxyArtworkForTag:tag]
                    name:[self proxyArtworkNameForTag:tag suffix:suffix]];
    }
    _directButton.selected = _selectedProxy == 1;
    _ghProxyButton.selected = _selectedProxy == 2;
    _craftProxyButton.selected = _selectedProxy == 3;
}

- (void)onActionTouchDown:(UIButton *)button
{
    if (button.tag == 1)
        [self setArtwork:_downloadArtwork name:@"download_label_active"];
    else
        [self setArtwork:_importArtwork name:@"import_label_active"];
}

- (void)onActionTouchCancelled:(UIButton *)button
{
    [self updateActionArtwork];
}

- (void)updateActionArtwork
{
    BOOL downloadActive = (_busy && _activeAction == 1) || _hoverAction == 1;
    BOOL importActive = (_busy && _activeAction == 2) || _importPickerOpen
        || _hoverAction == 2;
    [self setArtwork:_downloadArtwork name:downloadActive
        ? @"download_label_active" : @"download_label"];
    [self setArtwork:_importArtwork name:importActive
        ? @"import_label_active" : @"import_label"];
}

/* ---- UI state helpers ---- */

- (void)setBusy:(BOOL)busy
{
    _busy = busy;
    if (!busy)
    {
        _activeAction = 0;
        _importPickerOpen = NO;
    }
    /* Hold background execution time while a transfer runs: without it iOS
     * suspends the app seconds after backgrounding, the socket dies, the
     * download errors out, and the user comes back to a reset page where a
     * second tap starts the whole transfer over (two flows in parallel). */
    if (busy && _bgTask == UIBackgroundTaskInvalid)
    {
        __weak typeof(self) weakSelf = self;
        _bgTask = [[UIApplication sharedApplication]
            beginBackgroundTaskWithName:@"yosuga-transfer"
                      expirationHandler:^{
            typeof(self) blockSelf = weakSelf;
            if (blockSelf && blockSelf->_bgTask != UIBackgroundTaskInvalid)
            {
                [[UIApplication sharedApplication]
                    endBackgroundTask:blockSelf->_bgTask];
                blockSelf->_bgTask = UIBackgroundTaskInvalid;
            }
        }];
    }
    else if (!busy && _bgTask != UIBackgroundTaskInvalid)
    {
        [[UIApplication sharedApplication] endBackgroundTask:_bgTask];
        _bgTask = UIBackgroundTaskInvalid;
    }
    [UIApplication sharedApplication].idleTimerDisabled = busy;
    _downloadButton.enabled = !busy;
    _importButton.enabled = !busy;
    _directButton.enabled = !busy;
    _ghProxyButton.enabled = !busy;
    _craftProxyButton.enabled = !busy;
    [self updateProxyArtwork];
    [self updateActionArtwork];
    _progressLabel.hidden = !busy;
    _progressTrack.hidden = !busy;
    _progressFill.hidden = !busy;
    if (!busy)
    {
        CGRect frame = _progressFill.frame;
        frame.size.width = 0;
        _progressFill.frame = frame;
    }
}

- (void)setProgressText:(NSString *)text progress:(float)progress
{
    _progressLabel.text = text;
    CGFloat clamped = MAX(0.0, MIN(1.0, progress));
    CGRect frame = _progressFill.frame;
    frame.size.width = 1207.0 * clamped;
    _progressFill.frame = frame;
}

- (void)setMessage:(NSString *)message
{
    _messageLabel.text = message.length > 0 ? message : @" ";
}

- (void)finishWithResult:(int)result
{
    if (self.finished)
        return;
    self.finished = YES;
    self.result = result;
    [UIApplication sharedApplication].idleTimerDisabled = NO;
}

/* ---- download ---- */

- (void)onDownload
{
    if (_busy)
        return;
    _activeAction = 1;
    [self setMessage:@""];
    [self setBusy:YES];
    [self setProgressText:@"正在获取下载清单…" progress:0];
    [self downloadAll];
}

- (void)downloadAll
{
    NSString *baseUrl = [self effectiveBaseUrl];
    NSString *originalManifestUrl =
        [baseUrl stringByAppendingString:@"data-assets.json"];
    NSString *proxy = [self proxyPrefix];
    NSString *manifestUrl = proxy.length > 0
        ? [proxy stringByAppendingString:originalManifestUrl]
        : originalManifestUrl;
    IosLog([NSString stringWithFormat:@"downloadAll baseUrl=%@", baseUrl]);
    [self fetchJson:manifestUrl completion:^(id json, NSString *error) {
        if (!json || error.length > 0)
        {
            [self downloadFailed:[NSString stringWithFormat:
                @"获取数据清单失败：%@", error ?: @"HTTP 错误"]];
            return;
        }
        NSArray *assets = json[@"assets"];
        if (![assets isKindOfClass:NSArray.class] || assets.count == 0)
        {
            [self downloadFailed:@"无法读取下载清单（data-assets.json），请检查网络后重试"];
            return;
        }
        self->_assetList = [assets copy];
        self->_assetIndex = 0;
        self->_doneBytes = 0;
        self->_totalBytes = 0;
        self->_downloadStart = CFAbsoluteTimeGetCurrent();
        for (NSDictionary *a in assets)
        {
            NSNumber *size = a[@"size"];
            if ([size isKindOfClass:NSNumber.class])
                self->_totalBytes += size.longLongValue;
        }
        EnsureDir(CacheDirPath());
        IosLog([NSString stringWithFormat:@"manifest ok, assets=%lu totalBytes=%lld",
            (unsigned long)assets.count, self->_totalBytes]);
        /* Deleting a previous multi-GB data tree must not block the main
         * thread (watchdog); do the whole-tree swap on a worker queue. */
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            RemoveTree(DataDirPath());
            ClearDataComplete();
            dispatch_async(dispatch_get_main_queue(), ^{
                [self downloadNextAsset];
            });
        });
    }];
}

- (void)downloadNextAsset
{
    NSArray *assets = self.assetList;
    if (self.assetIndex >= assets.count)
    {
        [self dataInstalled];
        return;
    }
    NSDictionary *asset = assets[self.assetIndex];
    NSString *name = asset[@"name"];
    NSNumber *size = asset[@"size"];
    NSString *sha256 = asset[@"sha256"];
    NSString *originalUrl = [[self effectiveBaseUrl] stringByAppendingString:name];
    NSString *proxy = [self proxyPrefix];
    NSString *url = proxy.length > 0 ? [proxy stringByAppendingString:originalUrl]
                                     : originalUrl;

    NSString *tmp = [NSTemporaryDirectory()
        stringByAppendingPathComponent:[NSString stringWithFormat:@"krkr-dl-%@", name]];
    RemoveTree(tmp);
    IosLog([NSString stringWithFormat:@"downloading %@ (%@ bytes, url=%@)",
        name, size, url]);
    [self setProgressText:[NSString stringWithFormat:@"正在下载 %@", name] progress:0];

    NSURLSessionConfiguration *cfg =
        [NSURLSessionConfiguration defaultSessionConfiguration];
    cfg.timeoutIntervalForRequest = 300;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:cfg
        delegate:self delegateQueue:[NSOperationQueue mainQueue]];
    self.activeSession = session;
    self.activeTaskState = @{@"name": name, @"size": size ?: @0,
        @"sha256": sha256 ?: @"", @"tmp": tmp, @"url": url,
        @"retried": @NO};

    /* Split the asset into 8MB chunks and fetch 6 at a time with Range
     * requests, writing each response at its exact file offset. */
    RemoveTree([tmp stringByAppendingString:@".parts"]);
    if (![[NSFileManager defaultManager] createFileAtPath:tmp
        contents:nil attributes:nil])
    {
        [self downloadFailed:@"无法创建下载临时文件"];
        return;
    }
    self.chunkFile = [NSFileHandle fileHandleForWritingAtPath:tmp];
    if (!self.chunkFile)
    {
        [self downloadFailed:@"无法打开下载临时文件"];
        return;
    }
    long long total = size.longLongValue;
    const long long kChunk = 8LL * 1024 * 1024;
    self.chunkPlans = [NSMutableArray array];
    self.chunkReceivedTotal = 0;
    self.chunkFailed = NO;
    self.chunksRemaining = 0;
    for (long long start = 0; start < total; start += kChunk)
    {
        long long end = MIN(start + kChunk, total) - 1;
        [self.chunkPlans addObject:@{
            @"start": @(start), @"end": @(end), @"received": @0
        }];
        self.chunksRemaining++;
    }
    __block NSInteger launched = 0;
    [self.chunkPlans enumerateObjectsUsingBlock:
        ^(NSDictionary *plan, NSUInteger idx, BOOL *stop)
    {
        if (launched >= 6 || self.chunkFailed) { *stop = YES; return; }
        launched++;
        NSMutableURLRequest *req = [NSMutableURLRequest
            requestWithURL:[NSURL URLWithString:url]];
        NSString *range = [NSString stringWithFormat:@"bytes=%@-%@",
            plan[@"start"], plan[@"end"]];
        [req setValue:range forHTTPHeaderField:@"Range"];
        NSURLSessionDataTask *task = [session dataTaskWithRequest:req];
        NSMutableDictionary *m = [self.chunkPlans[idx] mutableCopy];
        m[@"task"] = task;
        self.chunkPlans[idx] = m;
        [task resume];
    }];
}

- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task
    didReceiveResponse:(NSURLResponse *)response
    completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler
{
    NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
    if (http.statusCode != 206 && !self.chunkFailed)
    {
        self.chunkFailed = YES;
        [session invalidateAndCancel];
        [self handleChunkFailure:[NSString stringWithFormat:@"HTTP %ld",
            (long)http.statusCode]];
    }
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task
    didReceiveData:(NSData *)data
{
    if (self.chunkFailed) return;
    NSInteger hit = -1;
    for (NSUInteger i = 0; i < self.chunkPlans.count; i++)
    {
        if (self.chunkPlans[i][@"task"] == task) { hit = (NSInteger)i; break; }
    }
    if (hit < 0) return;
    NSMutableDictionary *m = [self.chunkPlans[hit] mutableCopy];
    long long start = [m[@"start"] longLongValue];
    long long received = [m[@"received"] longLongValue];
    @try
    {
        /* seek+write (not writeData:atOffset:error: - that is iOS 13+).
         * Delegates run on the main queue, so seek and write are never
         * interleaved with another chunk's seek+write. */
        [self.chunkFile seekToFileOffset:(unsigned long long)(start + received)];
        [self.chunkFile writeData:data];
    }
    @catch (NSException *exc)
    {
        self.chunkFailed = YES;
        [session invalidateAndCancel];
        [self handleChunkFailure:exc.reason ?: @"写入失败"];
        return;
    }
    m[@"received"] = @(received + data.length);
    self.chunkPlans[hit] = m;
    self.chunkReceivedTotal += data.length;

    NSDictionary *st = self.activeTaskState;
    NSString *name = st[@"name"];
    long long doneTotal = self.doneBytes + self.chunkReceivedTotal;
    float pct = self.totalBytes > 0
        ? (float)((double)doneTotal * 100.0 / (double)self.totalBytes) : 0.0f;
    /* Progress text unified with the OHOS build:
     * "正在下载 <name>  <pct>%  <done> / <total>  (<speed>)". */
    NSTimeInterval elapsed = MAX(0.001, CFAbsoluteTimeGetCurrent() - self.downloadStart);
    NSString *speed = [[self fmtSize:(long long)(doneTotal / elapsed)]
        stringByAppendingString:@"/s"];
    [self setProgressText:[NSString stringWithFormat:
        @"正在下载 %@  %.0f%%  %@ / %@  (%@)", name, pct,
        [self fmtSize:doneTotal], [self fmtSize:self.totalBytes], speed]
        progress:MIN(0.99f, pct / 100.0f)];
}

/* One chunk finished (or errored): launch the next unlaunched chunk, and
 * when every chunk is done move to the verify+extract stage. */
- (void)chunkDidFinish:(NSURLSessionDataTask *)task error:(NSError *)error
{
    if (self.chunkFailed) return;
    NSInteger hit = -1;
    for (NSUInteger i = 0; i < self.chunkPlans.count; i++)
    {
        if (self.chunkPlans[i][@"task"] == task) { hit = (NSInteger)i; break; }
    }
    if (hit < 0) return;
    NSMutableDictionary *m = [self.chunkPlans[hit] mutableCopy];
    long long start = [m[@"start"] longLongValue];
    long long end = [m[@"end"] longLongValue];
    long long expected = end - start + 1;
    if (error || [m[@"received"] longLongValue] != expected)
    {
        self.chunkFailed = YES;
        [self.activeSession invalidateAndCancel];
        [self handleChunkFailure:error.localizedDescription ?: @"块不完整"];
        return;
    }
    /* launch the next pending chunk to keep 6 workers busy */
    for (NSUInteger i = 0; i < self.chunkPlans.count; i++)
    {
        if (!self.chunkPlans[i][@"task"])
        {
            NSDictionary *plan = self.chunkPlans[i];
            NSMutableURLRequest *req = [NSMutableURLRequest
                requestWithURL:[NSURL URLWithString:self.activeTaskState[@"url"]]];
            [req setValue:[NSString stringWithFormat:@"bytes=%@-%@",
                plan[@"start"], plan[@"end"]] forHTTPHeaderField:@"Range"];
            NSURLSessionDataTask *nt = [self.activeSession dataTaskWithRequest:req];
            NSMutableDictionary *nm = [self.chunkPlans[i] mutableCopy];
            nm[@"task"] = nt;
            self.chunkPlans[i] = nm;
            [nt resume];
            break;
        }
    }
    self.chunksRemaining--;
    if (self.chunksRemaining == 0)
    {
        NSDictionary *st = self.activeTaskState;
        [self.chunkFile closeFile];
        self.chunkFile = nil;
        [self verifyAndProcessAsset:st[@"tmp"] name:st[@"name"]
            sha256:st[@"sha256"] assetSize:st[@"size"]];
    }
}

- (void)handleChunkFailure:(NSString *)reason
{
    if (self.chunkFile)
    {
        [self.chunkFile closeFile];
        self.chunkFile = nil;
    }
    NSDictionary *st = self.activeTaskState;
    NSString *tmp = st[@"tmp"];
    if (tmp.length > 0) RemoveTree(tmp);
    if (![st[@"retried"] boolValue])
    {
        NSMutableDictionary *m = [st mutableCopy];
        m[@"retried"] = @YES;
        self.activeTaskState = m;
        [self setProgressText:@"下载失败，正在重试…" progress:0];
        [self downloadNextAsset];
        return;
    }
    [self downloadFailed:[NSString stringWithFormat:
        @"下载失败：%@", reason]];
}

- (void)verifyAndProcessAsset:(NSString *)tmp name:(NSString *)name
    sha256:(NSString *)sha256 assetSize:(NSNumber *)assetSize
{
    [self setProgressText:[NSString stringWithFormat:@"正在校验 %@", name] progress:0];
    /* sha256 over ~1.5 GB must NOT run on the main thread: it blocks the
     * run loop long enough for the iOS watchdog to kill the app (this was
     * the ~40% crash). Verify and extract on a background queue. */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (sha256.length > 0)
        {
            unsigned char digest[CC_SHA256_DIGEST_LENGTH];
            NSString *hashErr = nil;
            if (!SHA256OfFile(tmp, digest, &hashErr) ||
                ![[HexString(digest, CC_SHA256_DIGEST_LENGTH)
                    lowercaseString] isEqualToString:[sha256 lowercaseString]])
            {
                RemoveTree(tmp);
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self downloadFailed:@"下载校验失败（sha256 不匹配），请重试"];
                });
                return;
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [self setProgressText:[NSString stringWithFormat:@"正在解压 %@", name] progress:0];
            self.doneBytes += assetSize.longLongValue;
        });
        IosLog([NSString stringWithFormat:@"verified %@, extracting", name]);
        [self processArchive:tmp name:name];
    });
}

/* The former single-stream downloadTask flow is superseded by the
 * concurrent Range dataTask path above. */
- (void)URLSession:(NSURLSession *)session downloadTask:(NSURLSessionDownloadTask *)task
    didFinishDownloadingToURL:(NSURL *)location
{
    /* Not used anymore: downloads go through the Range dataTask path. */
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error
{
    if ([task isKindOfClass:[NSURLSessionDataTask class]]
        && self.activeSession == session)
    {
        [self chunkDidFinish:(NSURLSessionDataTask *)task error:error];
        return;
    }
    if (error && !self.finished)
    {
        [self handleChunkFailure:error.localizedDescription];
    }
}

- (void)downloadFailed:(NSString *)message
{
    IosLog([NSString stringWithFormat:@"FAILED: %@", message]);
    NSDictionary *st = self.activeTaskState;
    if (st)
    {
        NSString *tmp = st[@"tmp"];
        if (tmp.length > 0)
            RemoveTree(tmp);
    }
    RemoveTree(StagingPath());
    [self setMessage:message];
    [self setProgressText:message progress:0];
    [self setBusy:NO];
}

/* extraction progress: throttled main-queue updates */
static int ExtractProgressCb(void *ctx, int done, int total, const char *nameUtf8)
{
    if (done % 40 != 0 && done != total)
        return 1;
    TVPIOSBootstrapVC *vc = (__bridge TVPIOSBootstrapVC *)ctx;
    NSString *text = [NSString stringWithFormat:
        @"正在解压：%d / %d 个文件", done, total];
    dispatch_async(dispatch_get_main_queue(), ^{
        [vc setProgressText:text progress:0];
    });
    return 1;
}

- (void)processArchive:(NSString *)path name:(NSString *)name
{
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *staging = StagingPath();
        RemoveTree(staging);
        EnsureDir(staging);
        int rc = 0;
        char err[512] = {0};
        if ([name.lowercaseString hasSuffix:@".xp3"])
        {
            OHOSXp3ExtractResult xr;
            memset(&xr, 0, sizeof(xr));
            rc = OHOS_ExtractXp3(path.fileSystemRepresentation,
                staging.fileSystemRepresentation, ExtractProgressCb,
                (__bridge void *)self, &xr);
            if (rc != 0)
                snprintf(err, sizeof(err), "%s", xr.error);
        }
        else
        {
            rc = Krkr_ExtractZip(path.fileSystemRepresentation,
                staging.fileSystemRepresentation, ExtractProgressCb,
                (__bridge void *)self, err, sizeof(err));
        }
        NSString *errMsg = nil;
        NSString *cErr = rc != 0 ? [NSString stringWithUTF8String:err] : nil;
        BOOL ok = (rc == 0) && [self mergeOnMain:staging err:&errMsg];
        IosLog([NSString stringWithFormat:@"extract %@ rc=%d cErr=%@ mergeErr=%@",
            name, rc, cErr ?: @"", errMsg ?: @""]);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (ok)
            {
                RemoveTree(path);
                RemoveTree(staging);
                self.assetIndex = self.assetIndex + 1;
                [self downloadNextAsset];
            }
            else
            {
                RemoveTree(path);   /* downloaded/imported archive */
                RemoveTree(staging); /* partial extraction tree */
                NSString *detail = errMsg.length > 0 ? errMsg
                    : (cErr ?: @"");
                [self downloadFailed:[NSString stringWithFormat:
                    @"解压失败（%@）：%@", name, detail]];
            }
        });
    });
}

/* merge must run on the main queue via a synchronous wait (file moves are
 * cheap; the heavy work is the extraction above) */
- (BOOL)mergeOnMain:(NSString *)staging err:(NSString **)err
{
    __block BOOL ok = NO;
    __block NSString *e = nil;
    if ([NSThread isMainThread])
    {
        ok = MergeIntoDataDir(staging, &e);
    }
    else
    {
        dispatch_sync(dispatch_get_main_queue(), ^{
            ok = MergeIntoDataDir(staging, &e);
        });
    }
    if (err) *err = e;
    return ok;
}

- (void)dataInstalled
{
    MarkDataComplete();
    IosLog(@"data installed, engine starting");
    [self setMessage:@""];
    [self setBusy:NO];
    [self finishWithResult:1];
}

- (NSString *)fmtSize:(long long)bytes
{
    if (bytes >= 1024 * 1024 * 1024)
        return [NSString stringWithFormat:@"%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0)];
    return [NSString stringWithFormat:@"%.1f MB", bytes / (1024.0 * 1024.0)];
}

- (void)fetchJson:(NSString *)urlString completion:(void (^)(id, NSString *))completion
{
    NSURL *url = [NSURL URLWithString:urlString];
    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:url];
    req.timeoutInterval = 60;
    NSURLSession *session = [NSURLSession sharedSession];
    NSURLSessionDataTask *task = [session dataTaskWithRequest:req
        completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (error || !data)
                {
                    completion(nil, error.localizedDescription ?: @"网络错误");
                    return;
                }
                NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
                if (http.statusCode != 200)
                {
                    completion(nil, [NSString stringWithFormat:
                        @"HTTP %ld", (long)http.statusCode]);
                    return;
                }
                NSError *jsonErr = nil;
                id json = [NSJSONSerialization JSONObjectWithData:data
                    options:0 error:&jsonErr];
                if (!json)
                {
                    completion(nil, jsonErr.localizedDescription);
                    return;
                }
                completion(json, nil);
            });
        }];
    [task resume];
}

- (void)finishTask:(void (^)(void))work
{
    if (work)
        work();
}

/* ---- import ---- */

- (void)onImport
{
    if (_busy)
        return;
    _activeAction = 2;
    _importPickerOpen = YES;
    [self updateActionArtwork];
    [self setMessage:@""];
    NSArray *types = @[UTTypeZIP, UTTypeData];
    UIDocumentPickerViewController *picker =
        [[UIDocumentPickerViewController alloc]
            initForOpeningContentTypes:types asCopy:YES];
    picker.delegate = self;
    picker.allowsMultipleSelection = YES;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller
    didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
    if (urls.count == 0)
    {
        _importPickerOpen = NO;
        _activeAction = 0;
        [self updateActionArtwork];
        return;
    }
    IosLog([NSString stringWithFormat:@"import picked %lu file(s)", (unsigned long)urls.count]);
    _importPickerOpen = NO;
    _activeAction = 2;
    [self setBusy:YES];
    [self setProgressText:@"正在导入，请稍等" progress:0];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        RemoveTree(DataDirPath()); /* whole-tree swap, replaces ANY previous data */
        ClearDataComplete();
        [self importArchives:urls];
    });
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller
{
    _importPickerOpen = NO;
    _activeAction = 0;
    [self updateActionArtwork];
    [self setMessage:@"未选择文件"];
}

- (void)importArchives:(NSArray<NSURL *> *)urls
{
    IosLog(@"importArchives begin");
    EnsureDir(CacheDirPath());
    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableArray *local = [NSMutableArray array];
    for (NSURL *url in urls)
    {
        NSString *name = url.lastPathComponent;
        NSString *dst = [CacheDirPath() stringByAppendingPathComponent:name];
        RemoveTree(dst);
        NSError *err = nil;
        if (![fm copyItemAtURL:url toURL:[NSURL fileURLWithPath:dst] error:&err])
        {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self downloadFailed:[NSString stringWithFormat:
                    @"导入失败：%@", err.localizedDescription]];
            });
            return;
        }
        [local addObject:@{@"path": dst, @"name": name}];
    }
    /* staged imports extract one by one on the main thread for UI updates */
    for (NSDictionary *item in local)
    {
        dispatch_sync(dispatch_get_main_queue(), ^{
            [self setProgressText:[NSString stringWithFormat:
                @"正在导入 %@", item[@"name"]] progress:0];
        });
        NSString *staging = StagingPath();
        RemoveTree(staging);
        EnsureDir(staging);
        int rc = 0;
        char err[512] = {0};
        NSString *path = item[@"path"];
        if ([path.lowercaseString hasSuffix:@".xp3"])
        {
            OHOSXp3ExtractResult xr;
            memset(&xr, 0, sizeof(xr));
            rc = OHOS_ExtractXp3(path.fileSystemRepresentation,
                staging.fileSystemRepresentation, ExtractProgressCb,
                (__bridge void *)self, &xr);
            if (rc != 0)
                snprintf(err, sizeof(err), "%s", xr.error);
        }
        else
        {
            rc = Krkr_ExtractZip(path.fileSystemRepresentation,
                staging.fileSystemRepresentation, ExtractProgressCb,
                (__bridge void *)self, err, sizeof(err));
        }
        NSString *mergeErr = nil;
        NSString *cErr = rc != 0 ? [NSString stringWithUTF8String:err] : nil;
        BOOL ok = (rc == 0) && [self mergeOnMain:staging err:&mergeErr];
        IosLog([NSString stringWithFormat:@"import extract %@ rc=%d cErr=%@ mergeErr=%@",
            item[@"name"], rc, cErr ?: @"", mergeErr ?: @""]);
        RemoveTree(staging);
        RemoveTree(path);
        if (!ok)
        {
            NSString *detail = mergeErr.length > 0 ? mergeErr
                : (cErr ?: @"");
            dispatch_async(dispatch_get_main_queue(), ^{
                [self downloadFailed:[NSString stringWithFormat:
                    @"导入失败：%@", detail]];
            });
            return;
        }
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [self dataInstalled];
    });
}

@end

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int krkrsdl2_ios_run_bootstrap(void)
{
    @autoreleasepool {
        /* A previous crash may have left partial downloads/extractions
         * behind: they are never resumed, so drop them at startup. */
        RemoveTree(CacheDirPath());
        RemoveTree(StagingPath());
        {
            NSArray *tmpItems = [[NSFileManager defaultManager]
                contentsOfDirectoryAtPath:NSTemporaryDirectory() error:nil];
            for (NSString *item in tmpItems)
            {
                if ([item hasPrefix:@"krkr-dl-"])
                    RemoveTree([NSTemporaryDirectory()
                        stringByAppendingPathComponent:item]);
            }
        }
        IosLog(@"bootstrap start");
        if (GameDataReady())
            return 1;
        IosLog(@"showing bootstrap UI");

        TVPIOSBootstrapVC *vc = [[TVPIOSBootstrapVC alloc] init];
        UIWindowScene *scene = nil;
        for (UIScene *s in UIApplication.sharedApplication.connectedScenes)
        {
            if ([s isKindOfClass:UIWindowScene.class])
            {
                scene = (UIWindowScene *)s;
                break;
            }
        }
        UIWindow *window = scene
            ? [[UIWindow alloc] initWithWindowScene:scene]
            : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
        window.windowLevel = UIWindowLevelAlert + 2;
        window.rootViewController = vc;
        [window makeKeyAndVisible];

        while (!vc.finished)
        {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        }
        int result = vc.result;
        window.hidden = YES;
        window.rootViewController = nil;
        for (UIWindow *w in UIApplication.sharedApplication.windows)
        {
            if (w != window && w.windowLevel <= UIWindowLevelNormal)
            {
                [w makeKeyAndVisible];
                break;
            }
        }
        return result;
    }
}
