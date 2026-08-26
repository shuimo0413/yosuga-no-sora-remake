/* SPDX-License-Identifier: MIT */
/*
 * iOS bootstrap page, mirroring the OpenHarmony shell page:
 * - full-screen Background.png
 * - white title with a black outline, black body text
 * - download URL field + proxy field + three proxy buttons
 *   (直连 / gh-proxy / Craft-Hello Proxy)
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
 * used when present (the CI packer prefixes everything with data/). */
static BOOL MergeIntoDataDir(NSString *staging, NSString **errOut)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *dataDir = DataDirPath();
    NSString *src = staging;
    NSString *prefixed = [staging stringByAppendingPathComponent:@"data"];
    if ([fm fileExistsAtPath:[prefixed stringByAppendingPathComponent:@"startup.tjs"]])
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
@property (nonatomic, strong) NSURLSession *activeSession;
@property (nonatomic, strong) NSURLSessionDownloadTask *activeTask;
@property (nonatomic, copy) NSDictionary *activeTaskState;
@end

@implementation TVPIOSBootstrapVC
{
    UIImageView *_background;
    UILabel *_titleLabel;
    UILabel *_messageLabel;
    UILabel *_hintLabel;
    UITextField *_urlField;
    UITextField *_proxyField;
    UIButton *_downloadButton;
    UIButton *_importButton;
    UILabel *_progressLabel;
    UIProgressView *_progressBar;
    UIView *_container;
    BOOL _busy;
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
    return @"https://github.com/WarSkyGod/yosuga-no-sora-remake/releases/latest/download/";
}

- (NSString *)effectiveBaseUrl
{
    NSString *text = [_urlField.text stringByTrimmingCharactersInSet:
        NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (text.length > 0)
        return text;
    return [self defaultBaseUrl];
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

- (UIButton *)makeButton:(NSString *)title background:(NSUInteger)hex
{
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal];
    [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    b.backgroundColor = [self colorFromHex:hex];
    b.layer.cornerRadius = 6;
    b.clipsToBounds = YES;
    return b;
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

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;

    /* Background image (Bundle Resources/Background.png). */
    UIImage *bg = [UIImage imageWithContentsOfFile:
        [[NSBundle mainBundle] pathForResource:@"Background" ofType:@"png"]];
    _background = [[UIImageView alloc] initWithImage:bg];
    _background.contentMode = UIViewContentModeScaleAspectFill;
    _background.autoresizingMask =
        UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _background.frame = self.view.bounds;
    [self.view addSubview:_background];

    _container = [[UIView alloc] initWithFrame:CGRectZero];
    _container.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:_container];

    /* Title: white fill with a black OUTLINE ONLY. A negative
     * NSStrokeWidthAttributeName strokes centered on the glyph edge, so
     * the stroke bleeds into the glyph interior; use two stacked labels
     * instead: the bottom one is stroke-only (positive width), the top
     * one is the plain white fill. */
    _titleLabel = [[UILabel alloc] initWithFrame:CGRectZero];
    _titleLabel.font = [UIFont boldSystemFontOfSize:26];
    _titleLabel.textAlignment = NSTextAlignmentCenter;
    _titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    NSMutableAttributedString *stroke = [[NSMutableAttributedString alloc]
        initWithString:@"缘之空：高清重制"];
    [stroke addAttribute:NSForegroundColorAttributeName value:UIColor.whiteColor
                   range:NSMakeRange(0, stroke.length)];
    [stroke addAttribute:NSStrokeColorAttributeName value:UIColor.blackColor
                   range:NSMakeRange(0, stroke.length)];
    [stroke addAttribute:NSStrokeWidthAttributeName value:@(3.0)
                   range:NSMakeRange(0, stroke.length)];
    _titleLabel.attributedText = stroke;

    UILabel *titleFill = [[UILabel alloc] initWithFrame:CGRectZero];
    titleFill.font = [UIFont boldSystemFontOfSize:26];
    titleFill.textAlignment = NSTextAlignmentCenter;
    titleFill.textColor = UIColor.whiteColor;
    titleFill.text = @"缘之空：高清重制";
    titleFill.translatesAutoresizingMaskIntoConstraints = NO;
    [_container addSubview:_titleLabel];
    [_container addSubview:titleFill];
    [NSLayoutConstraint activateConstraints:@[
        [titleFill.topAnchor constraintEqualToAnchor:_titleLabel.topAnchor],
        [titleFill.leadingAnchor constraintEqualToAnchor:_titleLabel.leadingAnchor],
        [titleFill.trailingAnchor constraintEqualToAnchor:_titleLabel.trailingAnchor],
        [titleFill.bottomAnchor constraintEqualToAnchor:_titleLabel.bottomAnchor],
    ]];

    _messageLabel = [[UILabel alloc] initWithFrame:CGRectZero];
    _messageLabel.font = [UIFont systemFontOfSize:11];
    _messageLabel.textColor = UIColor.redColor;
    _messageLabel.textAlignment = NSTextAlignmentCenter;
    _messageLabel.numberOfLines = 6;
    _messageLabel.text = @" ";
    _messageLabel.translatesAutoresizingMaskIntoConstraints = NO;
    [_container addSubview:_messageLabel];

    _hintLabel = [[UILabel alloc] initWithFrame:CGRectZero];
    _hintLabel.font = [UIFont systemFontOfSize:13];
    _hintLabel.textColor = UIColor.blackColor;
    _hintLabel.textAlignment = NSTextAlignmentCenter;
    _hintLabel.numberOfLines = 0;
    _hintLabel.text = @"需要游戏数据（约 3.6 GB）：可在线下载，或从本地选择 zip 压缩包 / data.xp3 导入";
    _hintLabel.translatesAutoresizingMaskIntoConstraints = NO;
    [_container addSubview:_hintLabel];

    _urlField = [self makeField:@"下载地址（留空使用默认值）"];
    _urlField.translatesAutoresizingMaskIntoConstraints = NO;
    [_container addSubview:_urlField];

    _proxyField = [self makeField:@"加速代理前缀（留空=直连）"];
    _proxyField.translatesAutoresizingMaskIntoConstraints = NO;
    [_container addSubview:_proxyField];

    UIButton *directButton = [self makeButton:@"直连" background:0x666666];
    [directButton addTarget:self action:@selector(onDirect)
           forControlEvents:UIControlEventTouchUpInside];
    UIButton *ghProxyButton = [self makeButton:@"gh-proxy" background:0x2E5A88];
    [ghProxyButton addTarget:self action:@selector(onGhProxy)
            forControlEvents:UIControlEventTouchUpInside];
    UIButton *craftButton = [self makeButton:@"Craft-Hello Proxy" background:0x2E5A88];
    [craftButton addTarget:self action:@selector(onCraftProxy)
          forControlEvents:UIControlEventTouchUpInside];

    UIStackView *proxyRow = [[UIStackView alloc] initWithArrangedSubviews:
        @[directButton, ghProxyButton, craftButton]];
    proxyRow.axis = UILayoutConstraintAxisHorizontal;
    proxyRow.spacing = 8;
    proxyRow.distribution = UIStackViewDistributionFillEqually;
    proxyRow.translatesAutoresizingMaskIntoConstraints = NO;
    [_container addSubview:proxyRow];

    UILabel *urlHint = [[UILabel alloc] initWithFrame:CGRectZero];
    urlHint.font = [UIFont systemFontOfSize:11];
    urlHint.textColor = UIColor.blackColor;
    urlHint.textAlignment = NSTextAlignmentCenter;
    urlHint.numberOfLines = 0;
    urlHint.text = @"下载地址留空使用默认值；加速代理前缀会自动拼在原下载链接前";
    urlHint.translatesAutoresizingMaskIntoConstraints = NO;
    [_container addSubview:urlHint];

    _downloadButton = [self makeButton:@"下载游戏数据" background:0x3A5BA0];
    [_downloadButton addTarget:self action:@selector(onDownload)
              forControlEvents:UIControlEventTouchUpInside];
    _importButton = [self makeButton:@"从本地压缩包导入" background:0x3A5BA0];
    [_importButton addTarget:self action:@selector(onImport)
            forControlEvents:UIControlEventTouchUpInside];

    UIStackView *actionRow = [[UIStackView alloc] initWithArrangedSubviews:
        @[_downloadButton, _importButton]];
    actionRow.axis = UILayoutConstraintAxisHorizontal;
    actionRow.spacing = 12;
    actionRow.distribution = UIStackViewDistributionFillEqually;
    actionRow.translatesAutoresizingMaskIntoConstraints = NO;
    [_container addSubview:actionRow];

    _progressLabel = [[UILabel alloc] initWithFrame:CGRectZero];
    _progressLabel.font = [UIFont systemFontOfSize:14];
    _progressLabel.textColor = UIColor.whiteColor;
    _progressLabel.backgroundColor = [self colorFromHex:0x333333];
    _progressLabel.textAlignment = NSTextAlignmentCenter;
    _progressLabel.numberOfLines = 3;
    _progressLabel.layer.cornerRadius = 8;
    _progressLabel.clipsToBounds = YES;
    _progressLabel.hidden = YES;
    _progressLabel.translatesAutoresizingMaskIntoConstraints = NO;
    [_container addSubview:_progressLabel];

    _progressBar = [[UIProgressView alloc] initWithProgressViewStyle:
        UIProgressViewStyleDefault];
    _progressBar.hidden = YES;
    _progressBar.translatesAutoresizingMaskIntoConstraints = NO;
    [_container addSubview:_progressBar];

    NSDictionary *views = @{
        @"title": _titleLabel, @"msg": _messageLabel, @"hint": _hintLabel,
        @"url": _urlField, @"proxy": _proxyField, @"proxyRow": proxyRow,
        @"urlHint": urlHint, @"actions": actionRow,
        @"progressLabel": _progressLabel, @"progressBar": _progressBar
    };
    NSDictionary *metrics = @{@"fieldH": @36, @"btnH": @52, @"gap": @8};

    [NSLayoutConstraint activateConstraints:@[
        [_container.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [_container.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [_container.widthAnchor constraintEqualToConstant:560],
        [_titleLabel.topAnchor constraintEqualToAnchor:_container.topAnchor],
        [_titleLabel.leadingAnchor constraintEqualToAnchor:_container.leadingAnchor],
        [_titleLabel.trailingAnchor constraintEqualToAnchor:_container.trailingAnchor],
        [_messageLabel.topAnchor constraintEqualToAnchor:_titleLabel.bottomAnchor constant:6],
        [_messageLabel.leadingAnchor constraintEqualToAnchor:_container.leadingAnchor],
        [_messageLabel.trailingAnchor constraintEqualToAnchor:_container.trailingAnchor],
        [_hintLabel.topAnchor constraintEqualToAnchor:_messageLabel.bottomAnchor constant:6],
        [_hintLabel.leadingAnchor constraintEqualToAnchor:_container.leadingAnchor],
        [_hintLabel.trailingAnchor constraintEqualToAnchor:_container.trailingAnchor],
        [_urlField.topAnchor constraintEqualToAnchor:_hintLabel.bottomAnchor constant:10],
        [_urlField.leadingAnchor constraintEqualToAnchor:_container.leadingAnchor],
        [_urlField.trailingAnchor constraintEqualToAnchor:_container.trailingAnchor],
        [_urlField.heightAnchor constraintEqualToConstant:36],
        [_proxyField.topAnchor constraintEqualToAnchor:_urlField.bottomAnchor constant:8],
        [_proxyField.leadingAnchor constraintEqualToAnchor:_container.leadingAnchor],
        [_proxyField.trailingAnchor constraintEqualToAnchor:_container.trailingAnchor],
        [_proxyField.heightAnchor constraintEqualToConstant:36],
        [proxyRow.topAnchor constraintEqualToAnchor:_proxyField.bottomAnchor constant:8],
        [proxyRow.leadingAnchor constraintEqualToAnchor:_container.leadingAnchor],
        [proxyRow.trailingAnchor constraintEqualToAnchor:_container.trailingAnchor],
        [proxyRow.heightAnchor constraintEqualToConstant:32],
        [urlHint.topAnchor constraintEqualToAnchor:proxyRow.bottomAnchor constant:6],
        [urlHint.leadingAnchor constraintEqualToAnchor:_container.leadingAnchor],
        [urlHint.trailingAnchor constraintEqualToAnchor:_container.trailingAnchor],
        [actionRow.topAnchor constraintEqualToAnchor:urlHint.bottomAnchor constant:10],
        [actionRow.leadingAnchor constraintEqualToAnchor:_container.leadingAnchor],
        [actionRow.trailingAnchor constraintEqualToAnchor:_container.trailingAnchor],
        [actionRow.heightAnchor constraintEqualToConstant:52],
        [_progressLabel.topAnchor constraintEqualToAnchor:actionRow.bottomAnchor constant:14],
        [_progressLabel.leadingAnchor constraintEqualToAnchor:_container.leadingAnchor],
        [_progressLabel.trailingAnchor constraintEqualToAnchor:_container.trailingAnchor],
        [_progressBar.topAnchor constraintEqualToAnchor:_progressLabel.bottomAnchor constant:8],
        [_progressBar.leadingAnchor constraintEqualToAnchor:_container.leadingAnchor],
        [_progressBar.trailingAnchor constraintEqualToAnchor:_container.trailingAnchor],
        [_container.bottomAnchor constraintEqualToAnchor:_progressBar.bottomAnchor],
    ]];
    (void)metrics;
    (void)views;
}

/* ---- proxy buttons ---- */

- (void)onDirect { _proxyField.text = @""; }
- (void)onGhProxy { _proxyField.text = @"https://gh-proxy.cn/"; }
- (void)onCraftProxy { _proxyField.text = @"https://proxy.craft-hello.top/proxy/"; }

/* ---- UI state helpers ---- */

- (void)setBusy:(BOOL)busy
{
    _busy = busy;
    [UIApplication sharedApplication].idleTimerDisabled = busy;
    _downloadButton.enabled = !busy;
    _importButton.enabled = !busy;
    _progressLabel.hidden = !busy;
    _progressBar.hidden = !busy;
}

- (void)setProgressText:(NSString *)text progress:(float)progress
{
    _progressLabel.text = text;
    _progressBar.progress = progress;
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
    [self setMessage:@""];
    [self setBusy:YES];
    [self setProgressText:@"正在获取下载清单…" progress:0];
    [self downloadAll];
}

- (void)downloadAll
{
    NSString *baseUrl = [self effectiveBaseUrl];
    NSString *manifestUrl = [baseUrl stringByAppendingString:@"data-assets.json"];
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
    NSString *originalUrl = [[self effectiveBaseUrl] stringByAppendingFormat:@"/%@", name];
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
    NSURLSessionDownloadTask *task = [session downloadTaskWithURL:
        [NSURL URLWithString:url]];
    self.activeTask = task;
    self.activeTaskState = @{@"name": name, @"size": size ?: @0,
        @"sha256": sha256 ?: @"", @"tmp": tmp, @"url": url,
        @"retried": @NO};
    /* progress handled in the delegate below */
    [task resume];
}

- (void)URLSession:(NSURLSession *)session downloadTask:(NSURLSessionDownloadTask *)task
    didWriteData:(int64_t)bytesWritten totalBytesWritten:(int64_t)totalBytesWritten
    totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite
{
    NSDictionary *st = self.activeTaskState;
    NSString *name = st[@"name"];
    long long doneTotal = self.doneBytes + totalBytesWritten;
    float pct = self.totalBytes > 0
        ? (float)((double)doneTotal * 100.0 / (double)self.totalBytes) : 0.0f;
    [self setProgressText:[NSString stringWithFormat:
        @"正在下载 %@  %.0f%%  %@ / %@", name, pct,
        [self fmtSize:doneTotal], [self fmtSize:self.totalBytes]]
        progress:MIN(0.99f, pct / 100.0f)];
}

- (void)URLSession:(NSURLSession *)session downloadTask:(NSURLSessionDownloadTask *)task
    didFinishDownloadingToURL:(NSURL *)location
{
    NSDictionary *st = self.activeTaskState;
    NSString *name = st[@"name"];
    NSString *tmp = st[@"tmp"];
    NSString *sha256 = st[@"sha256"];
    NSError *err = nil;
    RemoveTree(tmp);
    /* The temporary download file is deleted once this delegate method
     * returns, so the rename must complete synchronously here. */
    if (![[NSFileManager defaultManager] moveItemAtURL:location
        toURL:[NSURL fileURLWithPath:tmp] error:&err])
    {
        [self downloadFailed:[NSString stringWithFormat:
            @"下载保存失败：%@", err.localizedDescription]];
        return;
    }
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
            self.doneBytes += [st[@"size"] longLongValue];
        });
        IosLog([NSString stringWithFormat:@"verified %@, extracting", name]);
        [self processArchive:tmp name:name];
    });
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error
{
    if (error && !self.finished)
    {
        NSDictionary *st = self.activeTaskState;
        if (![st[@"retried"] boolValue])
        {
            /* retry the same asset once before giving up */
            NSMutableDictionary *m = [st mutableCopy];
            m[@"retried"] = @YES;
            self.activeTaskState = m;
            [self setProgressText:@"下载失败，正在重试…" progress:0];
            [self downloadNextAsset];
            return;
        }
        [self downloadFailed:[NSString stringWithFormat:
            @"下载失败：%@", error.localizedDescription]];
    }
    [session invalidateAndCancel];
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
        return;
    IosLog([NSString stringWithFormat:@"import picked %lu file(s)", (unsigned long)urls.count]);
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

/* Engine-side logging into the same bootstrap.log (C interface). */
void krkrsdl2_ios_log(const char *message)
{
    IosLog([NSString stringWithUTF8String:message ? message : ""]);
}

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
