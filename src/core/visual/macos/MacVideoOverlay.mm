#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <strings.h>

#include "MacVideoOverlay.h"

static bool TVPMacUsesNativePixelCoordinates()
{
    const char *value = std::getenv("KRKRSDL2_MACOS_NATIVE_PIXELS");
    if(!value || !*value) return true;
    return strcasecmp(value, "0") != 0 &&
        strcasecmp(value, "false") != 0 &&
        strcasecmp(value, "no") != 0 &&
        strcasecmp(value, "off") != 0;
}

@interface TVPMovieView : NSView
@end

@implementation TVPMovieView
- (NSView *)hitTest:(NSPoint)point
{
    (void)point;
    return nil;
}
@end

@interface TVPMacMovieController : NSObject
- (instancetype)initWithPath:(NSString *)path
                      window:(NSWindow *)window
                     context:(void *)context
                    finished:(TVPMacVideoFinishedCallback)finished;
- (void)shutdown;
- (void)play;
- (void)pause;
- (void)stop;
- (void)rewind;
- (void)setMovieBoundsLeft:(int)left top:(int)top width:(int)width height:(int)height;
- (void)setMovieVisible:(BOOL)visible;
- (void)setMovieVolume:(float)volume;
- (void)setMovieRate:(float)rate;
- (void)setMovieTime:(double)seconds;
- (float)movieVolume;
- (float)movieRate;
- (BOOL)hasAudio;
- (int)videoWidth;
- (int)videoHeight;
- (double)frameRate;
- (double)duration;
- (double)currentTime;
@end

@implementation TVPMacMovieController {
    AVURLAsset *_asset;
    AVPlayerItem *_item;
    AVPlayer *_player;
    AVPlayerLayer *_playerLayer;
    TVPMovieView *_view;
    NSView *_contentView;
    id _endObserver;
    id _failureObserver;
    id _resizeObserver;
    void *_context;
    TVPMacVideoFinishedCallback _finished;
    BOOL _playing;
    BOOL _ended;
    BOOL _hasAudio;
    BOOL _hasCustomBounds;
    int _left;
    int _top;
    int _width;
    int _height;
    float _volume;
    float _rate;
}

- (instancetype)initWithPath:(NSString *)path
                      window:(NSWindow *)window
                     context:(void *)context
                    finished:(TVPMacVideoFinishedCallback)finished
{
    self = [super init];
    if(!self) return nil;
    if(path.length == 0 || ![[NSFileManager defaultManager] isReadableFileAtPath:path])
        return nil;

    NSURL *url = [NSURL fileURLWithPath:path isDirectory:NO];
    _asset = [AVURLAsset URLAssetWithURL:url options:nil];
    if(!_asset.playable || _asset.hasProtectedContent) return nil;

    _context = context;
    _finished = finished;
    _volume = 1.0f;
    _rate = 1.0f;
    _item = [AVPlayerItem playerItemWithAsset:_asset];
    _player = [AVPlayer playerWithPlayerItem:_item];
    _player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
    _player.volume = _volume;
    _hasAudio = [_asset tracksWithMediaType:AVMediaTypeAudio].count > 0;

    window = window ?: NSApp.keyWindow ?: NSApp.mainWindow;
    if(!window)
    {
        for(NSWindow *candidate in NSApp.windows)
        {
            if(candidate.isVisible)
            {
                window = candidate;
                break;
            }
        }
    }
    _contentView = window.contentView;
    if(!_contentView) return nil;

    _view = [[TVPMovieView alloc] initWithFrame:_contentView.bounds];
    _view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _view.wantsLayer = YES;
    _view.layer.backgroundColor = NSColor.blackColor.CGColor;

    _playerLayer = [AVPlayerLayer playerLayerWithPlayer:_player];
    _playerLayer.frame = _view.bounds;
    _playerLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    _playerLayer.videoGravity = AVLayerVideoGravityResizeAspect;
    [_view.layer addSublayer:_playerLayer];
    [_contentView addSubview:_view positioned:NSWindowAbove relativeTo:nil];

    NSNotificationCenter *notifications = [NSNotificationCenter defaultCenter];
    __weak TVPMacMovieController *weakSelf = self;
    _endObserver = [notifications
        addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                    object:_item
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *notification) {
                    (void)notification;
                    TVPMacMovieController *strongSelf = weakSelf;
                    if(!strongSelf) return;
                    strongSelf->_playing = NO;
                    strongSelf->_ended = YES;
                    if(strongSelf->_finished)
                        strongSelf->_finished(strongSelf->_context);
                }];
    _failureObserver = [notifications
        addObserverForName:AVPlayerItemFailedToPlayToEndTimeNotification
                    object:_item
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *notification) {
                    NSError *error = notification.userInfo[AVPlayerItemFailedToPlayToEndTimeErrorKey];
                    NSLog(@"krkrsdl2: movie playback failed: %@",
                          error.localizedDescription ?: @"unknown error");
                    TVPMacMovieController *strongSelf = weakSelf;
                    if(strongSelf) strongSelf->_playing = NO;
                }];

    _contentView.postsFrameChangedNotifications = YES;
    _resizeObserver = [notifications
        addObserverForName:NSViewFrameDidChangeNotification
                    object:_contentView
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *notification) {
                    (void)notification;
                    TVPMacMovieController *strongSelf = weakSelf;
                    if(strongSelf && strongSelf->_hasCustomBounds)
                        [strongSelf updateMovieFrame];
                }];
    return self;
}

- (AVAssetTrack *)videoTrack
{
    return [_asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
}

- (void)updateMovieFrame
{
    if(!_view || !_contentView || !_hasCustomBounds) return;

    NSRect contentBounds;
    NSRect movieFrame;
    if(TVPMacUsesNativePixelCoordinates())
    {
        contentBounds = [_contentView convertRectToBacking:_contentView.bounds];
        movieFrame = NSMakeRect(NSMinX(contentBounds) + _left,
            NSMaxY(contentBounds) - _top - _height, _width, _height);
        movieFrame = [_contentView convertRectFromBacking:movieFrame];
    }
    else
    {
        contentBounds = _contentView.bounds;
        movieFrame = NSMakeRect(NSMinX(contentBounds) + _left,
            NSMaxY(contentBounds) - _top - _height, _width, _height);
    }
    _view.frame = NSIntegralRect(movieFrame);
}

- (void)play
{
    if(!_player || _playing) return;
    if(_ended)
    {
        [_player seekToTime:kCMTimeZero
           toleranceBefore:kCMTimeZero
            toleranceAfter:kCMTimeZero];
        _ended = NO;
    }
    _playing = YES;
    [_player play];
    _player.rate = _rate;
}

- (void)pause
{
    [_player pause];
    _playing = NO;
}

- (void)stop
{
    [_player pause];
    [_player seekToTime:kCMTimeZero
       toleranceBefore:kCMTimeZero
        toleranceAfter:kCMTimeZero];
    _playing = NO;
    _ended = NO;
}

- (void)rewind
{
    [_player seekToTime:kCMTimeZero
       toleranceBefore:kCMTimeZero
        toleranceAfter:kCMTimeZero];
    _ended = NO;
    if(_playing) _player.rate = _rate;
}

- (void)setMovieBoundsLeft:(int)left top:(int)top width:(int)width height:(int)height
{
    _left = left;
    _top = top;
    _width = std::max(width, 0);
    _height = std::max(height, 0);
    _hasCustomBounds = YES;
    _view.autoresizingMask = NSViewNotSizable;
    [self updateMovieFrame];
}

- (void)setMovieVisible:(BOOL)visible
{
    _view.hidden = !visible;
}

- (void)setMovieVolume:(float)volume
{
    _volume = std::max(0.0f, std::min(volume, 1.0f));
    _player.volume = _volume;
}

- (void)setMovieRate:(float)rate
{
    _rate = std::max(rate, 0.01f);
    if(_playing) _player.rate = _rate;
}

- (void)setMovieTime:(double)seconds
{
    if(!_player || !std::isfinite(seconds)) return;
    double movieDuration = [self duration];
    seconds = std::max(seconds, 0.0);
    if(movieDuration > 0.0) seconds = std::min(seconds, movieDuration);
    CMTime time = CMTimeMakeWithSeconds(seconds, 600);
    [_player seekToTime:time toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
    _ended = NO;
}

- (float)movieVolume { return _volume; }
- (float)movieRate { return _rate; }
- (BOOL)hasAudio { return _hasAudio; }

- (int)videoWidth
{
    AVAssetTrack *track = [self videoTrack];
    if(!track) return 0;
    CGRect transformed = CGRectApplyAffineTransform(
        CGRectMake(0, 0, track.naturalSize.width, track.naturalSize.height),
        track.preferredTransform);
    return (int)std::lround(std::fabs(transformed.size.width));
}

- (int)videoHeight
{
    AVAssetTrack *track = [self videoTrack];
    if(!track) return 0;
    CGRect transformed = CGRectApplyAffineTransform(
        CGRectMake(0, 0, track.naturalSize.width, track.naturalSize.height),
        track.preferredTransform);
    return (int)std::lround(std::fabs(transformed.size.height));
}

- (double)frameRate
{
    AVAssetTrack *track = [self videoTrack];
    if(!track) return 0.0;
    if(track.nominalFrameRate > 0.0f) return track.nominalFrameRate;
    double frameDuration = CMTimeGetSeconds(track.minFrameDuration);
    return std::isfinite(frameDuration) && frameDuration > 0.0
        ? 1.0 / frameDuration : 0.0;
}

- (double)duration
{
    CMTime time = _item.duration;
    if(!CMTIME_IS_NUMERIC(time)) time = _asset.duration;
    double seconds = CMTimeGetSeconds(time);
    return std::isfinite(seconds) && seconds > 0.0 ? seconds : 0.0;
}

- (double)currentTime
{
    double seconds = CMTimeGetSeconds(_player.currentTime);
    return std::isfinite(seconds) && seconds > 0.0 ? seconds : 0.0;
}

- (void)shutdown
{
    _playing = NO;
    [_player pause];
    NSNotificationCenter *notifications = [NSNotificationCenter defaultCenter];
    if(_endObserver) [notifications removeObserver:_endObserver];
    if(_failureObserver) [notifications removeObserver:_failureObserver];
    if(_resizeObserver) [notifications removeObserver:_resizeObserver];
    _endObserver = nil;
    _failureObserver = nil;
    _resizeObserver = nil;
    _playerLayer.player = nil;
    [_playerLayer removeFromSuperlayer];
    [_view removeFromSuperview];
    _playerLayer = nil;
    _view = nil;
    _contentView = nil;
    _player = nil;
    _item = nil;
    _asset = nil;
    _finished = nullptr;
    _context = nullptr;
}

- (void)dealloc
{
    [self shutdown];
}
@end

struct TVPMacVideoHandle {
    __strong TVPMacMovieController *controller;
};

void *TVPMacVideoCreate(const char *path, void *nativeWindow, void *context,
                        TVPMacVideoFinishedCallback finished)
{
    if(!path) return nullptr;
    NSString *moviePath = [[NSString alloc] initWithUTF8String:path];
    if(!moviePath) return nullptr;
    NSWindow *window = (__bridge NSWindow *)nativeWindow;
    TVPMacMovieController *controller = [[TVPMacMovieController alloc]
        initWithPath:moviePath window:window context:context finished:finished];
    if(!controller) return nullptr;
    TVPMacVideoHandle *handle = new TVPMacVideoHandle();
    handle->controller = controller;
    return handle;
}

static TVPMacMovieController *TVPMacController(void *handle)
{
    return handle ? static_cast<TVPMacVideoHandle *>(handle)->controller : nil;
}

void TVPMacVideoDestroy(void *handle)
{
    if(!handle) return;
    TVPMacVideoHandle *movie = static_cast<TVPMacVideoHandle *>(handle);
    [movie->controller shutdown];
    movie->controller = nil;
    delete movie;
}

void TVPMacVideoPlay(void *handle) { [TVPMacController(handle) play]; }
void TVPMacVideoPause(void *handle) { [TVPMacController(handle) pause]; }
void TVPMacVideoStop(void *handle) { [TVPMacController(handle) stop]; }
void TVPMacVideoRewind(void *handle) { [TVPMacController(handle) rewind]; }
void TVPMacVideoSetBounds(void *handle, int left, int top, int width, int height)
{
    [TVPMacController(handle) setMovieBoundsLeft:left top:top width:width height:height];
}
void TVPMacVideoSetScreenGeometry(void *handle, int windowWidth, int windowHeight)
{
    (void)handle;
    (void)windowWidth;
    (void)windowHeight;
    /* macOS maps overlay bounds through the content view's backing store;
       the engine's logical window size is not needed there. */
}
void TVPMacVideoSetVisible(void *handle, int visible)
{
    [TVPMacController(handle) setMovieVisible:visible != 0];
}
void TVPMacVideoSetVolume(void *handle, float volume)
{
    [TVPMacController(handle) setMovieVolume:volume];
}
void TVPMacVideoSetRate(void *handle, float rate)
{
    [TVPMacController(handle) setMovieRate:rate];
}
void TVPMacVideoSetTime(void *handle, double seconds)
{
    [TVPMacController(handle) setMovieTime:seconds];
}
float TVPMacVideoGetVolume(void *handle)
{
    return [TVPMacController(handle) movieVolume];
}
float TVPMacVideoGetRate(void *handle)
{
    return [TVPMacController(handle) movieRate];
}
int TVPMacVideoHasAudio(void *handle)
{
    return [TVPMacController(handle) hasAudio] ? 1 : 0;
}
int TVPMacVideoGetWidth(void *handle)
{
    return [TVPMacController(handle) videoWidth];
}
int TVPMacVideoGetHeight(void *handle)
{
    return [TVPMacController(handle) videoHeight];
}
double TVPMacVideoGetFPS(void *handle)
{
    return [TVPMacController(handle) frameRate];
}
double TVPMacVideoGetDuration(void *handle)
{
    return [TVPMacController(handle) duration];
}
double TVPMacVideoGetTime(void *handle)
{
    return [TVPMacController(handle) currentTime];
}
