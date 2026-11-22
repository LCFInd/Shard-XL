/* SPDX-License-Identifier: GPL-3.0-only */
/* The Mac: a Cocoa window that shows the core's framebuffer, Core Audio out (the default output, 48 kHz float;
 * the system converts to the device's rate), Core MIDI in from every source (the Axiom included, plugged in
 * before or after start), the Mac's own decoders for AIFF, MP3, M4A, FLAC, CAF..., and files dropped on the window. */
#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "xl.h"

static uint32_t *g_fb;
static int g_w, g_h, g_scale = 2;

/* ---- the platform calls ---- */
double plat_time(void)
{
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom * 1e-9;
}
void plat_mkdir(const char *path) { mkdir(path, 0755); }
int plat_list_dir(const char *path, char names[][256], int *isdir, int max)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;
    if (!d) return -1;
    while ((e = readdir(d)) && n < max) {
        char full[2048];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (stat(full, &st) != 0) continue;
        snprintf(names[n], 256, "%s", e->d_name);
        isdir[n] = S_ISDIR(st.st_mode);
        n++;
    }
    closedir(d);
    return n;
}
int plat_choose_folder(char *out, int outlen)
{
    @autoreleasepool {
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.canChooseDirectories = YES;
        p.canChooseFiles = NO;
        p.allowsMultipleSelection = NO;
        p.message = @"Choose a folder of samples";
        if ([p runModal] == NSModalResponseOK && p.URLs.count > 0) {
            snprintf(out, (size_t)outlen, "%s", p.URLs[0].path.fileSystemRepresentation);
            return 1;
        }
    }
    return 0;
}
int plat_decode(const char *path, float **inter, int *frames, int *rate, int *channels)
{
    ExtAudioFileRef f = NULL;
    AudioStreamBasicDescription in, cl;
    UInt32 sz = sizeof in;
    SInt64 len = 0;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    float *buf;
    SInt64 got = 0;
    int ch;
    if (!url) return -1;
    if (ExtAudioFileOpenURL(url, &f) != noErr) { CFRelease(url); return -1; }
    CFRelease(url);
    if (ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileDataFormat, &sz, &in) != noErr) { ExtAudioFileDispose(f); return -1; }
    sz = sizeof len;
    ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileLengthFrames, &sz, &len);
    ch = (int)in.mChannelsPerFrame;
    if (ch < 1) ch = 1;
    if (ch > 2) ch = 2;
    memset(&cl, 0, sizeof cl);
    cl.mSampleRate = in.mSampleRate;
    cl.mFormatID = kAudioFormatLinearPCM;
    cl.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    cl.mBitsPerChannel = 32;
    cl.mChannelsPerFrame = (UInt32)ch;
    cl.mFramesPerPacket = 1;
    cl.mBytesPerFrame = cl.mBytesPerPacket = 4 * (UInt32)ch;
    if (ExtAudioFileSetProperty(f, kExtAudioFileProperty_ClientDataFormat, sizeof cl, &cl) != noErr) { ExtAudioFileDispose(f); return -1; }
    if (len <= 0) len = (SInt64)(in.mSampleRate * 600);
    if (len > (SInt64)(in.mSampleRate * 600)) len = (SInt64)(in.mSampleRate * 600);
    buf = malloc(sizeof(float) * (size_t)len * (size_t)ch + 16);
    while (got < len) {
        AudioBufferList bl;
        UInt32 n = (UInt32)((len - got) > 65536 ? 65536 : (len - got));
        bl.mNumberBuffers = 1;
        bl.mBuffers[0].mNumberChannels = (UInt32)ch;
        bl.mBuffers[0].mDataByteSize = n * 4 * (UInt32)ch;
        bl.mBuffers[0].mData = buf + got * ch;
        if (ExtAudioFileRead(f, &n, &bl) != noErr || n == 0) break;
        got += n;
    }
    ExtAudioFileDispose(f);
    if (got <= 0) { free(buf); return -1; }
    *inter = buf; *frames = (int)got; *rate = (int)lrint(in.mSampleRate); *channels = ch;
    return 0;
}

/* ---- audio ---- */
static AudioComponentInstance g_unit;
static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                          UInt32 frames, AudioBufferList *io)
{
    (void)ref; (void)flags; (void)ts; (void)bus;
    xl_render((float *)io->mBuffers[0].mData, (int)frames, 2);
    return noErr;
}
static void audio_start(void)
{
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    AudioStreamBasicDescription f;
    AURenderCallbackStruct cb = {render_cb, NULL};
    UInt32 bufsz = 128;
    if (!c || AudioComponentInstanceNew(c, &g_unit) != noErr) { NSLog(@"SHARD XL: no audio output"); return; }
    memset(&f, 0, sizeof f);
    f.mSampleRate = XL_SR;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mBitsPerChannel = 32; f.mChannelsPerFrame = 2; f.mFramesPerPacket = 1;
    f.mBytesPerFrame = f.mBytesPerPacket = 8;
    AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AudioUnitSetProperty(g_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    AudioUnitSetProperty(g_unit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &bufsz, sizeof bufsz);
    if (AudioUnitInitialize(g_unit) != noErr || AudioOutputUnitStart(g_unit) != noErr) NSLog(@"SHARD XL: audio would not start");
}

/* ---- MIDI ---- */
static MIDIClientRef g_client;
static MIDIPortRef g_port;
static NSMutableSet *g_connected;
static uint8_t g_sx[64];
static int g_sxn;
static void mmc(void)   /* MIDI Machine Control (the Axiom's transport buttons may send it) */
{
    if (g_sxn >= 4 && g_sx[0] == 0x7F && g_sx[2] == 0x06) {
        int c = g_sx[3];
        if (c == 0x02 || c == 0x03) xl_midi_in(0xFA, 0, 0);
        else if (c == 0x01 || c == 0x09) xl_midi_in(0xFC, 0, 0);
        else if (c == 0x06) xl_midi_in(0xF5, 0, 0);
    }
}
static void ump_packet(const UInt32 *w, UInt32 n)
{
    UInt32 i = 0;
    while (i < n) {
        UInt32 a = w[i], type = a >> 28, sz = (type <= 2) ? 1 : (type <= 4) ? 2 : 4;
        if (type == 2) xl_midi_in((uint8_t)(a >> 16), (uint8_t)((a >> 8) & 0x7F), (uint8_t)(a & 0x7F));
        else if (type == 1) {
            uint8_t st = (uint8_t)(a >> 16);
            if (st == 0xFA || st == 0xFB) xl_midi_in(0xFA, 0, 0);
            else if (st == 0xFC) xl_midi_in(0xFC, 0, 0);
        } else if (type == 3 && i + 1 < n) {
            UInt32 b = w[i + 1], form = (a >> 20) & 0xF, cnt = (a >> 16) & 0xF, k;
            uint8_t by[6] = {(uint8_t)(a >> 8), (uint8_t)a, (uint8_t)(b >> 24), (uint8_t)(b >> 16), (uint8_t)(b >> 8), (uint8_t)b};
            if (form == 0 || form == 1) g_sxn = 0;
            for (k = 0; k < cnt && k < 6; k++) if (g_sxn < 64) g_sx[g_sxn++] = by[k] & 0x7F;
            if (form == 0 || form == 3) mmc();
        }
        i += sz;
    }
}
static void midi_connect_all(void)
{
    ItemCount n = MIDIGetNumberOfSources(), i;
    if (!g_connected) g_connected = [NSMutableSet set];
    for (i = 0; i < n; i++) {
        MIDIEndpointRef src = MIDIGetSource(i);
        SInt32 uid = 0;
        MIDIObjectGetIntegerProperty(src, kMIDIPropertyUniqueID, &uid);
        if ([g_connected containsObject:@(uid)]) continue;
        if (MIDIPortConnectSource(g_port, src, NULL) == noErr) [g_connected addObject:@(uid)];
    }
}
static void midi_start(void)
{
    if (MIDIClientCreateWithBlock(CFSTR("SHARD XL"), &g_client, ^(const MIDINotification *msg) {
            if (msg->messageID == kMIDIMsgSetupChanged) dispatch_async(dispatch_get_main_queue(), ^{ midi_connect_all(); });
        }) != noErr) { NSLog(@"SHARD XL: no MIDI"); return; }
    if (MIDIInputPortCreateWithProtocol(g_client, CFSTR("SHARD XL in"), kMIDIProtocol_1_0, &g_port,
            ^(const MIDIEventList *list, void *refcon) {
                const MIDIEventPacket *p = &list->packet[0];
                UInt32 k;
                (void)refcon;
                for (k = 0; k < list->numPackets; k++) {
                    ump_packet(p->words, p->wordCount);
                    p = MIDIEventPacketNext(p);
                }
            }) != noErr) { NSLog(@"SHARD XL: no MIDI input port"); return; }
    midi_connect_all();
}

/* ---- the window ---- */
static int keymap(unsigned short code)
{
    static const signed char ANSI[128] = {
        'a', 's', 'd', 'f', 'h', 'g', 'z', 'x', 'c', 'v', 0, 'b', 'q', 'w', 'e', 'r', 'y', 't', '1', '2', '3', '4', '6', '5',
        '=', '9', '7', '-', '8', '0', ']', 'o', 'u', '[', 'i', 'p', 13, 'l', 'j', '\'', 'k', ';', '\\', ',', '/', 'n', 'm', '.',
        9, 32, '`', 127, 0, 27};
    if (code == 123) return K_LEFT;
    if (code == 124) return K_RIGHT;
    if (code == 125) return K_DOWN;
    if (code == 126) return K_UP;
    return code < 128 ? ANSI[code] : 0;
}
static int mods_of(NSEventModifierFlags f)
{
    return ((f & NSEventModifierFlagShift) ? MOD_SHIFT : 0) | ((f & NSEventModifierFlagOption) ? MOD_ALT : 0) |
           ((f & NSEventModifierFlagCommand) ? MOD_CMD : 0) | ((f & NSEventModifierFlagControl) ? MOD_CTRL : 0);
}

@interface XLView : NSView
@end
@implementation XLView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (NSPoint)logical:(NSEvent *)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSRect b = self.bounds;
    return NSMakePoint(p.x * 1280.0 / b.size.width, p.y * 720.0 / b.size.height);
}
- (void)drawRect:(NSRect)r
{
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, g_fb, (size_t)g_w * g_h * 4, NULL);
    CGImageRef img = CGImageCreate((size_t)g_w, (size_t)g_h, 8, 32, (size_t)g_w * 4, cs,
                                   (CGBitmapInfo)kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst,
                                   dp, NULL, false, kCGRenderingIntentDefault);
    NSRect b = self.bounds;
    (void)r;
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, b.size.height);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextDrawImage(ctx, CGRectMake(0, 0, b.size.width, b.size.height), img);
    CGContextRestoreGState(ctx);
    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
}
- (void)mouseDown:(NSEvent *)e { NSPoint p = [self logical:e]; xl_ui_mouse(MS_DOWN, (float)p.x, (float)p.y, mods_of(e.modifierFlags), (int)e.clickCount); }
- (void)mouseDragged:(NSEvent *)e { NSPoint p = [self logical:e]; xl_ui_mouse(MS_DRAG, (float)p.x, (float)p.y, mods_of(e.modifierFlags), 0); }
- (void)mouseUp:(NSEvent *)e { NSPoint p = [self logical:e]; xl_ui_mouse(MS_UP, (float)p.x, (float)p.y, mods_of(e.modifierFlags), 0); }
- (void)scrollWheel:(NSEvent *)e
{
    NSPoint p = [self logical:e];
    float k = e.hasPreciseScrollingDeltas ? 0.2f : 2.0f;
    xl_ui_scroll((float)e.scrollingDeltaX * k, (float)e.scrollingDeltaY * k, (float)p.x, (float)p.y, mods_of(e.modifierFlags));
}
- (void)keyDown:(NSEvent *)e
{
    int k = keymap(e.keyCode);
    if (e.isARepeat && k != K_LEFT && k != K_RIGHT && k != K_UP && k != K_DOWN) return;
    if (k) xl_ui_key(1, k, mods_of(e.modifierFlags));
}
- (void)keyUp:(NSEvent *)e { int k = keymap(e.keyCode); if (k) xl_ui_key(0, k, mods_of(e.modifierFlags)); }
- (void)flagsChanged:(NSEvent *)e { atomic_store(&XL.shift_held, (e.modifierFlags & NSEventModifierFlagShift) ? 1 : 0); }
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)s { (void)s; return NSDragOperationCopy; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)s
{
    NSArray *urls = [s.draggingPasteboard readObjectsForClasses:@[ [NSURL class] ]
                                                          options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    NSPoint p = [self convertPoint:s.draggingLocation fromView:nil];
    NSRect b = self.bounds;
    float x = (float)(p.x * 1280.0 / b.size.width), y = (float)(p.y * 720.0 / b.size.height);
    for (NSURL *u in urls) { xl_ui_drop(u.path.fileSystemRepresentation, x, y); break; }
    return YES;
}
@end

@interface XLApp : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(strong) NSWindow *win;
@property(strong) XLView *view;
@property(strong) NSTimer *timer;
@end
@implementation XLApp
- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    NSRect r = NSMakeRect(0, 0, 1280, 720);
    NSString *home = [NSHomeDirectory() stringByAppendingPathComponent:@"Music/SHARD-XL"];
    (void)n;
    self.win = [[NSWindow alloc] initWithContentRect:r
                                           styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                     NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
    self.win.title = @"SHARD XL";
    self.win.contentAspectRatio = NSMakeSize(16, 9);
    self.win.contentMinSize = NSMakeSize(960, 540);
    self.win.backgroundColor = NSColor.blackColor;
    self.win.delegate = self;
    self.win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    self.view = [[XLView alloc] initWithFrame:r];
    [self.view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    self.win.contentView = self.view;
    g_scale = self.win.backingScaleFactor >= 1.5 ? 2 : 1;
    g_w = 1280 * g_scale; g_h = 720 * g_scale;
    g_fb = calloc((size_t)g_w * g_h, 4);
    xl_app_init(home.fileSystemRepresentation, g_scale);
    audio_start();
    midi_start();
    [self.win center];
    [self.win makeKeyAndOrderFront:nil];
    [self.win makeFirstResponder:self.view];
    [NSApp activateIgnoringOtherApps:YES];
    self.timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *t) {
        (void)t;
        xl_ui_frame(g_fb, g_w, g_h, plat_time());
        [self.view setNeedsDisplay:YES];
    }];
    [[NSRunLoop currentRunLoop] addTimer:self.timer forMode:NSRunLoopCommonModes];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { (void)a; return YES; }
- (void)applicationWillTerminate:(NSNotification *)n
{
    (void)n;
    xl_ui_quit();
    if (g_unit) { AudioOutputUnitStop(g_unit); AudioUnitUninitialize(g_unit); }
}
@end

int main(int argc, const char **argv)
{
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        XLApp *d = [XLApp new];
        NSMenu *bar = [NSMenu new], *am = [NSMenu new], *wm = [NSMenu new];
        NSMenuItem *ai = [NSMenuItem new], *wi = [NSMenuItem new];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [am addItemWithTitle:@"About SHARD XL" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
        [am addItem:[NSMenuItem separatorItem]];
        [am addItemWithTitle:@"Hide SHARD XL" action:@selector(hide:) keyEquivalent:@"h"];
        [am addItemWithTitle:@"Quit SHARD XL" action:@selector(terminate:) keyEquivalent:@"q"];
        ai.submenu = am;
        [bar addItem:ai];
        wm.title = @"Window";
        [wm addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@""];
        {
            NSMenuItem *fs = [wm addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
            fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
        }
        wi.submenu = wm;
        [bar addItem:wi];
        app.mainMenu = bar;
        app.delegate = d;
        [app run];
    }
    return 0;
}
