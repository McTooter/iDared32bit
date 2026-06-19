//! App picker GUI.
//!
//! This also includes a license text viewer. The license text viewer is needed
//! on Android, where the command-line way to view license text doesn't exist.

use crate::bundle::Bundle;
use crate::frameworks::core_graphics::cg_bitmap_context::{
    CGBitmapContextCreate, CGBitmapContextCreateImage,
};
use crate::frameworks::core_graphics::cg_color_space::CGColorSpaceCreateDeviceRGB;
use crate::frameworks::core_graphics::cg_context::{
    CGContextFillRect, CGContextRelease, CGContextScaleCTM, CGContextSetRGBFillColor,
    CGContextTranslateCTM,
};
use crate::frameworks::core_graphics::cg_image::{self, kCGImageAlphaPremultipliedLast};
use crate::frameworks::core_graphics::{CGFloat, CGPoint, CGRect, CGSize};
use crate::frameworks::foundation::ns_run_loop::run_run_loop_single_iteration;
use crate::frameworks::foundation::{ns_string, NSUInteger};
use crate::frameworks::uikit::ui_font::{
    UITextAlignmentCenter, UITextAlignmentLeft, UITextAlignmentRight,
};
use crate::frameworks::uikit::ui_graphics::{UIGraphicsPopContext, UIGraphicsPushContext};
use crate::frameworks::uikit::ui_view::ui_control::ui_button::{
    UIButtonTypeCustom, UIButtonTypeRoundedRect,
};
use crate::frameworks::uikit::ui_view::ui_control::{
    UIControlEventTouchUpInside, UIControlEventValueChanged, UIControlStateNormal,
};
use crate::fs::BundleData;
use crate::image::Image;
use crate::mem::Ptr;
use crate::objc::{
    id, msg, msg_class, nil, objc_classes, release, AnyHostObject, ClassExports, HostObject,
    NSZonePtr,
};
use crate::options::Options;
use crate::paths;
use crate::window::DeviceOrientation;
use crate::Environment;
use std::ffi::OsStr;
use std::num::NonZeroU32;
use std::path::{Path, PathBuf};

struct AppInfo {
    path: PathBuf,
    display_name: String,
    icon: Option<Image>,
    /// `NSString*`
    display_name_ns_string: Option<id>,
    /// `UIImage*`
    icon_ui_image: Option<id>,
}

pub fn app_picker(options: Options) -> Result<(PathBuf, Vec<String>), String> {
    let apps_dir = paths::user_data_base_path().join(paths::APPS_DIR);

    let apps: Result<Vec<AppInfo>, String> = if !apps_dir.is_dir() {
        Err(format!("The {} directory couldn't be found. Check you're running touchHLE from the right directory.", paths::APPS_DIR))
    } else {
        enumerate_apps(&apps_dir)
            .map_err(|err| {
                format!(
                    "Couldn't get list of apps in the {} directory: {}.",
                    paths::APPS_DIR,
                    err
                )
            })
            .and_then(|apps| {
                if apps.is_empty() {
                    Err(format!(
                        "No apps were found in the {} directory.",
                        paths::APPS_DIR
                    ))
                } else {
                    Ok(apps)
                }
            })
    };

    show_app_picker_gui(options, apps)
}

fn enumerate_apps(apps_dir: &Path) -> Result<Vec<AppInfo>, std::io::Error> {
    let mut apps = Vec::new();
    for app in std::fs::read_dir(apps_dir)? {
        let app_path = app?.path();
        if app_path.extension() != Some(OsStr::new("app"))
            && app_path.extension() != Some(OsStr::new("ipa"))
        {
            continue;
        }

        // TODO: avoid loading the whole FS somehow?
        let (bundle, fs) = match BundleData::open_any(&app_path).and_then(|bundle_data| {
            Bundle::new_bundle_and_fs_from_host_path(bundle_data, /* read_only_mode: */ true)
        }) {
            Ok(ok) => ok,
            Err(e) => {
                log!(
                    "Warning: couldn't open app bundle {}: {} (skipping)",
                    app_path.display(),
                    e
                );
                continue;
            }
        };

        // TODO: what if this crashes?
        let display_name = bundle.display_name().to_owned();

        let icon = match bundle.load_icon(&fs) {
            Ok(icon) => Some(icon),
            Err(e) => {
                log!("Warning: couldn't load icon for app bundle {}: {} (displaying placeholder instead)", app_path.display(), e);
                None
            }
        };

        apps.push(AppInfo {
            path: app_path,
            display_name,
            icon,
            display_name_ns_string: None,
            icon_ui_image: None,
        });
    }

    apps.sort_by_key(|app| app.display_name.to_uppercase());

    Ok(apps)
}

#[derive(Default)]
struct AppPickerDelegateHostObject {
    grid_tapped_point: Option<CGPoint>,
    requested_page: Option<usize>,
    prev_page: bool,
    next_page: bool,
    copyright_show: bool,
    copyright_hide: bool,
    copyright_prev: bool,
    copyright_next: bool,
    quick_options_show: bool,
    quick_options_hide: bool,
    scale_hack_default: bool,
    scale_hack1: bool,
    scale_hack2: bool,
    scale_hack3: bool,
    scale_hack4: bool,
    orientation_default: bool,
    orientation_portrait_upside_down: bool,
    orientation_landscape_left: bool,
    orientation_landscape_right: bool,
    analog_stick_tilt_controls: Option<bool>,
    network: Option<bool>,
    fullscreen: Option<bool>,
}
impl HostObject for AppPickerDelegateHostObject {}

struct AppPickerGridViewHostObject {
    superclass: crate::frameworks::uikit::ui_view::UIViewHostObject,
    delegate: id,
    start_point: Option<CGPoint>,
    icon_buttons: Vec<id>,
    highlighted_button: id,
}

impl HostObject for AppPickerGridViewHostObject {
    fn as_superclass<'a>(&'a self) -> Option<&'a (dyn AnyHostObject + 'static)> {
        Some(&self.superclass)
    }
    fn as_superclass_mut<'a>(&'a mut self) -> Option<&'a mut (dyn AnyHostObject + 'static)> {
        Some(&mut self.superclass)
    }
}
impl Default for AppPickerGridViewHostObject {
    fn default() -> Self {
        Self {
            superclass: crate::frameworks::uikit::ui_view::UIViewHostObject::default(),
            delegate: nil,
            start_point: None,
            icon_buttons: Vec::new(),
            highlighted_button: nil,
        }
    }
}

struct AppPickerPageControlHostObject {
    superclass: crate::frameworks::uikit::ui_view::UIViewHostObject,
    delegate: id,
    number_of_pages: usize,
    current_page: usize,
}
impl Default for AppPickerPageControlHostObject {
    fn default() -> Self {
        Self {
            superclass: crate::frameworks::uikit::ui_view::UIViewHostObject::default(),
            delegate: nil,
            number_of_pages: 0,
            current_page: 0,
        }
    }
}
impl HostObject for AppPickerPageControlHostObject {
    fn as_superclass<'a>(&'a self) -> Option<&'a (dyn AnyHostObject + 'static)> {
        Some(&self.superclass)
    }
    fn as_superclass_mut<'a>(&'a mut self) -> Option<&'a mut (dyn AnyHostObject + 'static)> {
        Some(&mut self.superclass)
    }
}

pub const DYLIB: crate::dyld::HostDylib = crate::dyld::HostDylib {
    // Not a real iOS dylib obviously. This shouldn't really be in the list of
    // dylibs if we can avoid it somehow (TODO?).
    path: "/.touchHLE/AppPickerHelpers.dylib",
    aliases: &[],
    class_exports: &[CLASSES],
    constant_exports: &[],
    function_exports: &[],
};

/// Be careful! These classes go in the normal class list, just like everything
/// else, so an app could try to instantiate them. Don't give them special
/// powers that could be exploited!
const CLASSES: ClassExports = objc_classes! {

(env, this, _cmd);

@implementation _touchHLE_AppPickerDelegate: NSObject

- (())gridTappedAt:(CGPoint)point {
    let host_obj = env.objc.borrow_mut::<AppPickerDelegateHostObject>(this);
    host_obj.grid_tapped_point = Some(point);
}

- (())pageSelected:(NSUInteger)page {
    let host_obj = env.objc.borrow_mut::<AppPickerDelegateHostObject>(this);
    host_obj.requested_page = Some(page as usize);
}

- (())prevPage {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).prev_page = true;
}

- (())nextPage {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).next_page = true;
}

- (())copyrightInfoShow {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).copyright_show = true;
}
- (())copyrightInfoHide {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).copyright_hide = true;
}
- (())copyrightInfoPrevPage {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).copyright_prev = true;
}
- (())copyrightInfoNextPage {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).copyright_next = true;
}

- (())quickOptionsShow {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).quick_options_show = true;
}
- (())quickOptionsHide {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).quick_options_hide = true;
}
- (())scaleHackDefault {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).scale_hack_default = true;
}
- (())scaleHack1 {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).scale_hack1 = true;
}
- (())scaleHack2 {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).scale_hack2 = true;
}
- (())scaleHack3 {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).scale_hack3 = true;
}
- (())scaleHack4 {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).scale_hack4 = true;
}
- (())orientationDefault {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).orientation_default = true;
}
- (())orientationPortraitUpsideDown {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).orientation_portrait_upside_down = true;
}
- (())orientationLandscapeLeft {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).orientation_landscape_left = true;
}
- (())orientationLandscapeRight {
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).orientation_landscape_right = true;
}
- (())analogStickTiltControls:(id)switch { // UISwitch*
    let switch_state: bool = msg![env; switch isOn];
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).analog_stick_tilt_controls = Some(switch_state);
}
- (())network:(id)switch { // UISwitch*
    let switch_state: bool = msg![env; switch isOn];
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).network = Some(switch_state);
}
- (())fullscreen:(id)switch { // UISwitch*
    let switch_state: bool = msg![env; switch isOn];
    env.objc.borrow_mut::<AppPickerDelegateHostObject>(this).fullscreen = Some(switch_state);
}

- (())openFileManager {
    // Assert (see above).
    let _ = env.objc.borrow_mut::<AppPickerDelegateHostObject>(this);

    match paths::url_for_opening_user_data_dir() {
        Ok(url) => {
            // Our `openURL:` implementation is bypassed because it doesn't
            // allow non-web URLs.
            let url_res = crate::window::open_url(env, &url);
            if let Err(e) = url_res {
                echo!("Couldn't open file manager at {:?}: {}", url, e);
            } else {
                echo!("Opened file manager at {:?}.", url);
                if std::env::consts::OS != "ios" {
                    echo!("Exiting.");
                    std::process::exit(0);
                }
            }
        },
        Err(e) => echo!("Couldn't open file manager: {}", e),
    }
}

- (())visitWebsite {
    // Assert (see above).
    let _ = env.objc.borrow_mut::<AppPickerDelegateHostObject>(this);

    if let Err(e) = crate::window::open_url(env, "https://iDared32bit-emu.com/") {
        echo!("Couldn't open iDared32bit-emu.com: {}", e);
    }
}

@end

@implementation _touchHLE_AppPickerPageControl: UIView

+ (id)allocWithZone:(NSZonePtr)_zone {
    let host_object = Box::<AppPickerPageControlHostObject>::default();
    env.objc.alloc_object(this, host_object, &mut env.mem)
}

- (())setNumberOfPages:(NSUInteger)n {
    env.objc.borrow_mut::<AppPickerPageControlHostObject>(this).number_of_pages = n as usize;
    // Like UIPageControl's hidesForSinglePage: one dot says nothing.
    () = msg![env; this setHidden:(n <= 1)];
    () = msg![env; this setNeedsDisplay];
}
- (())setCurrentPage:(NSUInteger)n {
    env.objc.borrow_mut::<AppPickerPageControlHostObject>(this).current_page = n as usize;
    () = msg![env; this setNeedsDisplay];
}

- (())setDelegate:(id)delegate {
    env.objc.borrow_mut::<AppPickerPageControlHostObject>(this).delegate = delegate;
}

- (())touchesEnded:(id)touches withEvent:(id)_event {
    let touch: id = msg![env; touches anyObject];
    let point: CGPoint = msg![env; touch locationInView:this];
    let bounds: CGRect = msg![env; this bounds];
    let (number_of_pages, current_page, delegate) = {
        let host_obj = env.objc.borrow::<AppPickerPageControlHostObject>(this);
        (host_obj.number_of_pages, host_obj.current_page, host_obj.delegate)
    };

    if number_of_pages == 0 || delegate == nil { return; }

    let dot_spacing: CGFloat = 15.0;
    let total_width = (number_of_pages as f32 - 1.0) * dot_spacing;
    let start_x = bounds.size.width / 2.0 - total_width / 2.0;

    // A tap on a dot goes to its page. The dots are too small to hit
    // reliably, so like UIPageControl, a tap anywhere else goes one page
    // back or forward, depending on which side of the current dot it's on.
    let nearest = ((point.x - start_x) / dot_spacing).round();
    if nearest >= 0.0 && (nearest as usize) < number_of_pages
        && (point.x - (start_x + nearest * dot_spacing)).abs() <= dot_spacing / 2.0 {
        () = msg![env; delegate pageSelected:(nearest as NSUInteger)];
    } else if point.x < start_x + (current_page as f32) * dot_spacing {
        () = msg![env; delegate prevPage];
    } else {
        () = msg![env; delegate nextPage];
    }
}

- (())drawRect:(CGRect)_rect {
    use crate::frameworks::core_graphics::cg_context::{CGContextFillRect, CGContextSetRGBFillColor};
    let context = crate::frameworks::uikit::ui_graphics::UIGraphicsGetCurrentContext(env);
    let bounds: CGRect = msg![env; this bounds];
    let &AppPickerPageControlHostObject { number_of_pages, current_page, .. } = env.objc.borrow(this);

    if number_of_pages == 0 { return; }

    let dot_spacing = 15.0;
    let total_width = (number_of_pages as f32 - 1.0) * dot_spacing;
    let start_x = bounds.size.width / 2.0 - total_width / 2.0;
    let y = (bounds.size.height / 2.0).round();

    for i in 0..number_of_pages {
        let is_current = i == current_page;
        let size = if is_current { 7.0 } else { 5.0 };
        let alpha = if is_current { 1.0 } else { 0.5 };
        CGContextSetRGBFillColor(env, context, 1.0, 1.0, 1.0, alpha);

        let x = (start_x + (i as f32) * dot_spacing).round();
        let dot_rects = [
            CGRect { origin: CGPoint { x: x - size/2.0 + 1.0, y: y - size/2.0 }, size: CGSize { width: size - 2.0, height: size } },
            CGRect { origin: CGPoint { x: x - size/2.0, y: y - size/2.0 + 1.0 }, size: CGSize { width: size, height: size - 2.0 } },
        ];
        for r in dot_rects {
            CGContextFillRect(env, context, r);
        }
    }
}

@end

@implementation _touchHLE_AppPickerGridView: UIView

+ (id)allocWithZone:(NSZonePtr)_zone {
    let host_object = Box::<AppPickerGridViewHostObject>::default();
    env.objc.alloc_object(this, host_object, &mut env.mem)
}

- (())touchesBegan:(id)touches withEvent:(id)_event {
    let touch: id = msg![env; touches anyObject];
    let point: CGPoint = msg![env; touch locationInView:this];

    let (buttons, previous) = {
        let host_obj = env.objc.borrow_mut::<AppPickerGridViewHostObject>(this);
        host_obj.start_point = Some(point);
        (host_obj.icon_buttons.clone(), std::mem::take(&mut host_obj.highlighted_button))
    };
    // A new touch (e.g. a second finger) replaces the old one: un-highlight
    // its icon, or it would stay half-transparent.
    if previous != nil {
        () = msg![env; previous setAlpha:(1.0 as CGFloat)];
    }

    for &button in &buttons {
        let frame: CGRect = msg![env; button frame];
        if point.x >= frame.origin.x && point.x < frame.origin.x + frame.size.width &&
           point.y >= frame.origin.y && point.y < frame.origin.y + frame.size.height {
            () = msg![env; button setAlpha:(0.5 as CGFloat)];
            let host_obj = env.objc.borrow_mut::<AppPickerGridViewHostObject>(this);
            host_obj.highlighted_button = button;
            break;
        }
    }
}

- (())touchesMoved:(id)touches withEvent:(id)_event {
    let touch: id = msg![env; touches anyObject];
    let point: CGPoint = msg![env; touch locationInView:this];

    let (start_point, highlighted_button) = {
        let host_obj = env.objc.borrow::<AppPickerGridViewHostObject>(this);
        (host_obj.start_point, host_obj.highlighted_button)
    };
    if let Some(start_point) = start_point {
        let dx = point.x - start_point.x;
        if dx.abs() > 50.0 && highlighted_button != nil {
            () = msg![env; highlighted_button setAlpha:(1.0 as CGFloat)];
            let host_obj = env.objc.borrow_mut::<AppPickerGridViewHostObject>(this);
            host_obj.highlighted_button = nil;
        }
    }
}

- (())touchesEnded:(id)touches withEvent:(id)_event {
    let touch: id = msg![env; touches anyObject];
    let point: CGPoint = msg![env; touch locationInView:this];

    let (delegate, start_point, highlighted_button) = {
        let host_obj = env.objc.borrow::<AppPickerGridViewHostObject>(this);
        (host_obj.delegate, host_obj.start_point, host_obj.highlighted_button)
    };

    if let Some(start_point) = start_point {
        let dx = point.x - start_point.x;
        if dx > 50.0 {
            () = msg![env; delegate prevPage];
        } else if dx < -50.0 {
            () = msg![env; delegate nextPage];
        } else if highlighted_button != nil {
            // Only launch the icon that was pressed, and only if the finger
            // is still on it: not whichever icon the finger ends up on.
            let frame: CGRect = msg![env; highlighted_button frame];
            if point.x >= frame.origin.x && point.x < frame.origin.x + frame.size.width &&
               point.y >= frame.origin.y && point.y < frame.origin.y + frame.size.height {
                () = msg![env; delegate gridTappedAt:point];
            }
        }
    }

    if highlighted_button != nil {
        () = msg![env; highlighted_button setAlpha:(1.0 as CGFloat)];
    }
    let host_obj = env.objc.borrow_mut::<AppPickerGridViewHostObject>(this);
    host_obj.start_point = None;
    host_obj.highlighted_button = nil;
}

- (())touchesCancelled:(id)_touches withEvent:(id)_event {
    // Forget the touch without acting on it, and un-highlight its icon.
    let highlighted_button = {
        let host_obj = env.objc.borrow_mut::<AppPickerGridViewHostObject>(this);
        host_obj.start_point = None;
        std::mem::take(&mut host_obj.highlighted_button)
    };
    if highlighted_button != nil {
        () = msg![env; highlighted_button setAlpha:(1.0 as CGFloat)];
    }
}

@end

};

fn show_app_picker_gui(
    options: Options,
    apps: Result<Vec<AppInfo>, String>,
) -> Result<(PathBuf, Vec<String>), String> {
    let icon = {
        let bytes: &[u8] = match crate::branding() {
            "" => include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"), "/res/icon.png")),
            "UNOFFICIAL" => include_bytes!(concat!(
                env!("CARGO_MANIFEST_DIR"),
                "/res/icon_unofficial.png"
            )),
            "PREVIEW" => {
                include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"), "/res/icon_preview.png"))
            }
            _ => panic!(),
        };
        let mut image = Image::from_bytes(bytes).unwrap();
        // should match Bundle::load_icon()
        image.round_corners(
            (10.0 / 57.0) * (image.dimensions().0 as f32),
            /* four_corners: */ true,
            /* add_sheen: */ true,
        );
        image
    };
    let environment = Environment::new_without_app(options, icon)?;
    Ok(environment.run_app_picker(|env| app_picker_inner(env, apps)))
}

fn app_picker_inner(
    env: &mut Environment,
    mut apps: Result<Vec<AppInfo>, String>,
) -> (PathBuf, Vec<String>) {
    let mut option_args = Vec::new();
    // Note that objects are generally not released in this code, because they
    // don't need to be: the entire Environment is thrown away at the end.

    // Bypassing UIApplicationMain!
    let ui_application: id = msg_class![env; UIApplication new];
    let delegate = env
        .objc
        .get_known_class("_touchHLE_AppPickerDelegate", &mut env.mem);
    let delegate = env.objc.alloc_object(
        delegate,
        Box::<AppPickerDelegateHostObject>::default(),
        &mut env.mem,
    );
    () = msg![env; ui_application setDelegate:delegate];

    let screen: id = msg_class![env; UIScreen mainScreen];
    let bounds: CGRect = msg![env; screen bounds];

    let window: id = msg_class![env; UIWindow alloc];
    let window: id = msg![env; window initWithFrame:bounds];

    let app_frame: CGRect = msg![env; screen applicationFrame];
    let main_view: id = msg_class![env; UIView alloc];
    let main_view: id = msg![env; main_view initWithFrame:app_frame];
    () = msg![env; window addSubview:main_view];

    // Wallpaper
    let mut found_wallpaper = false;
    let mut have_wallpaper = false;
    for candidate in paths::WALLPAPER_FILES {
        let candidate = paths::user_data_base_path().join(candidate);
        if !candidate.exists() {
            continue;
        }
        found_wallpaper = true;

        let image = match std::fs::read(&candidate) {
            Ok(image) => image,
            Err(e) => {
                log!("Warning: couldn't read {}: {}", candidate.display(), e);
                break;
            }
        };
        let image = match Image::from_bytes(&image) {
            Ok(image) => image,
            Err(e) => {
                log!("Warning: couldn't decode {}: {}", candidate.display(), e);
                break;
            }
        };

        let image = cg_image::from_image(env, image);
        let image: id = msg_class![env; UIImage imageWithCGImage:image];
        let wallpaper: id = msg_class![env; UIImageView alloc];
        let wallpaper: id = msg![env; wallpaper initWithImage:image];
        () = msg![env; wallpaper setFrame:(CGRect {
            origin: CGPoint {
                x: 0.0,
                y: 0.0,
            },
            size: app_frame.size,
        })];
        () = msg![env; wallpaper setAlpha:(0.5 as CGFloat)];
        () = msg![env; main_view addSubview:wallpaper];
        have_wallpaper = true;
        break;
    }
    if !found_wallpaper {
        let CGSize { width, height } = app_frame.size;
        log!(
            "No wallpaper found; filename can be one of: {}; ideal size is {}×{} pixels",
            paths::WALLPAPER_FILES.join(", "),
            width,
            height,
        );
    }

    // Version label
    {
        let label_frame = CGRect {
            origin: CGPoint {
                x: 0.0,
                y: app_frame.size.height - 20.0,
            },
            size: CGSize {
                width: app_frame.size.width - 5.0,
                height: 15.0,
            },
        };
        let label: id = msg_class![env; UILabel alloc];
        let label: id = msg![env; label initWithFrame:label_frame];
        let text = ns_string::from_rust_string(
            env,
            format!(
                "iDared 32bit {}{}{}",
                crate::branding(),
                if crate::branding().is_empty() {
                    ""
                } else {
                    " "
                },
                crate::DISPLAY_VERSION
            ),
        );
        () = msg![env; label setText:text];
        () = msg![env; label setTextAlignment:UITextAlignmentRight];
        let font_size: CGFloat = 12.0;
        let font: id = msg_class![env; UIFont systemFontOfSize:font_size];
        () = msg![env; label setFont:font];
        let text_color: id = if have_wallpaper {
            msg_class![env; UIColor whiteColor]
        } else {
            msg_class![env; UIColor lightGrayColor]
        };
        () = msg![env; label setTextColor:text_color];
        let bg_color: id = msg_class![env; UIColor clearColor];
        () = msg![env; label setBackgroundColor:bg_color];
        () = msg![env; main_view addSubview:label];
    }

    let brand_color: id = if crate::branding() == "UNOFFICIAL" {
        msg_class![env; UIColor redColor]
    } else {
        msg_class![env; UIColor grayColor]
    };

    for i in 1..=7 {
        let label_frame = CGRect {
            origin: CGPoint {
                x: 0.0,
                y: (app_frame.size.height / 8.0) * (i as f32) - 25.0,
            },
            size: CGSize {
                width: app_frame.size.width,
                height: 50.0,
            },
        };
        let label: id = msg_class![env; UILabel alloc];
        let label: id = msg![env; label initWithFrame:label_frame];
        let text = ns_string::from_rust_string(env, crate::branding().to_owned());
        () = msg![env; label setText:text];
        () = msg![env; label setTextAlignment:(if i % 2 == 0 { UITextAlignmentLeft } else { UITextAlignmentRight })];
        let font_size: CGFloat = 48.0;
        let font: id = msg_class![env; UIFont systemFontOfSize:font_size];
        () = msg![env; label setFont:font];
        () = msg![env; label setTextColor:brand_color];
        let bg_color: id = msg_class![env; UIColor clearColor];
        () = msg![env; label setBackgroundColor:bg_color];
        () = msg![env; main_view addSubview:label];
    }

    let divider = app_frame.size.height - 100.0;

    let mut icon_grid_stuff = match &mut apps {
        Ok(ref mut apps) => {
            let mut icon_grid_stuff = make_icon_grid(
                env,
                delegate,
                main_view,
                app_frame,
                apps.len(),
                have_wallpaper,
            );
            update_icon_grid(env, &mut icon_grid_stuff, apps, 0);
            Some(icon_grid_stuff)
        }
        Err(e) => {
            let label_frame = CGRect {
                origin: CGPoint { x: 10.0, y: 10.0 },
                size: CGSize {
                    width: app_frame.size.width - 20.0,
                    height: divider - 20.0,
                },
            };
            let label: id = msg_class![env; UILabel alloc];
            let label: id = msg![env; label initWithFrame:label_frame];
            let text = ns_string::from_rust_string(env, e.clone());
            () = msg![env; label setText:text];
            () = msg![env; label setTextAlignment:UITextAlignmentCenter];
            () = msg![env; label setNumberOfLines:0]; // unlimited
            let text_color: id = msg_class![env; UIColor lightGrayColor];
            () = msg![env; label setTextColor:text_color];
            let bg_color: id = msg_class![env; UIColor clearColor];
            () = msg![env; label setBackgroundColor:bg_color];
            () = msg![env; main_view addSubview:label];
            None
        }
    };

    let buttons_row_center = divider + (app_frame.size.height - divider) / 4.0;
    let buttons_row2_center = divider + (app_frame.size.height - divider) / 1.6;
    make_button_row(
        env,
        delegate,
        main_view,
        app_frame.size,
        buttons_row_center,
        &[
            ("File manager", "openFileManager"),
            ("Quick options", "quickOptionsShow"),
        ],
        None,
    );
    make_button_row(
        env,
        delegate,
        main_view,
        app_frame.size,
        buttons_row2_center,
        &[
            ("Copyright info", "copyrightInfoShow"),
            ("iDared 32bit Code", "visitWebsite"),
        ],
        None,
    );

    let copyright_info_text = crate::licenses::get_text();
    let mut copyright_info_stuff = setup_copyright_info(env, delegate, main_view, app_frame);
    let mut copyright_info_page_idx = 0;

    let quick_options_stuff = setup_quick_options(env, delegate, main_view, app_frame);
    let mut quick_options_scale_hack: Option<NonZeroU32> = None;
    let mut quick_options_fullscreen: Option<()> = None;
    let mut quick_options_orientation: Option<DeviceOrientation> = None;
    let mut quick_options_analog_stick_tilt_controls = true;
    let mut quick_options_network = false;

    fn update_quick_option_buttons(env: &mut Environment, buttons: &[id], selected_idx: usize) {
        for (idx, &button) in buttons.iter().enumerate() {
            let color: id = if idx == selected_idx {
                msg_class![env; UIColor magentaColor]
            } else {
                msg_class![env; UIColor grayColor]
            };
            () = msg![env; button setBackgroundColor:color];
        }
    }
    fn update_scale_hack_buttons(env: &mut Environment, buttons: &[id], value: Option<NonZeroU32>) {
        update_quick_option_buttons(env, buttons, value.map_or(0, |v| v.get() as usize));
    }
    fn update_orientation_buttons(
        env: &mut Environment,
        buttons: &[id],
        value: Option<DeviceOrientation>,
    ) {
        update_quick_option_buttons(
            env,
            buttons,
            value.map_or(0, |v| match v {
                DeviceOrientation::LandscapeLeft => 1,
                DeviceOrientation::LandscapeRight => 2,
                DeviceOrientation::PortraitUpsideDown => 3,
                _ => panic!(),
            }),
        );
    }
    update_scale_hack_buttons(
        env,
        &quick_options_stuff.scale_hack_buttons,
        quick_options_scale_hack,
    );
    update_orientation_buttons(
        env,
        &quick_options_stuff.orientation_buttons,
        quick_options_orientation,
    );

    () = msg![env; window makeKeyAndVisible];

    let main_run_loop: id = msg_class![env; NSRunLoop mainRunLoop];
    let mut current_page_idx = 0;
    // If an app is picked, this loop returns. If the user quits touchHLE, the
    // process exits.
    let app_path = loop {
        run_run_loop_single_iteration(env, main_run_loop);
        let host_obj = env.objc.borrow_mut::<AppPickerDelegateHostObject>(delegate);

        if std::mem::take(&mut host_obj.prev_page) {
            if current_page_idx > 0 {
                current_page_idx -= 1;
                let grid_stuff = icon_grid_stuff.as_mut().unwrap();
                update_icon_grid(env, grid_stuff, apps.as_mut().unwrap(), current_page_idx);
                () = msg![env; (grid_stuff.page_control) setCurrentPage:(current_page_idx as NSUInteger)];
            }
            continue;
        }
        if let Some(idx) = std::mem::take(&mut host_obj.requested_page) {
            let grid_stuff = icon_grid_stuff.as_mut().unwrap();
            if idx < grid_stuff.pages.len() && idx != current_page_idx {
                current_page_idx = idx;
                update_icon_grid(env, grid_stuff, apps.as_mut().unwrap(), current_page_idx);
                () = msg![env; (grid_stuff.page_control) setCurrentPage:(current_page_idx as NSUInteger)];
            }
            continue;
        }
        if std::mem::take(&mut host_obj.next_page) {
            if current_page_idx + 1 < icon_grid_stuff.as_ref().unwrap().pages.len() {
                current_page_idx += 1;
                let grid_stuff = icon_grid_stuff.as_mut().unwrap();
                update_icon_grid(env, grid_stuff, apps.as_mut().unwrap(), current_page_idx);
                () = msg![env; (grid_stuff.page_control) setCurrentPage:(current_page_idx as NSUInteger)];
            }
            continue;
        }
        if let Some(point) = std::mem::take(&mut host_obj.grid_tapped_point) {
            let grid_stuff = icon_grid_stuff.as_mut().unwrap();
            let app_idx_range = grid_stuff.pages[current_page_idx].clone();
            let mut app_idx = None;

            for (i, &(button, _label)) in grid_stuff.icon_buttons_and_labels.iter().enumerate() {
                let frame: CGRect = msg![env; button frame];
                if point.x >= frame.origin.x
                    && point.x < frame.origin.x + frame.size.width
                    && point.y >= frame.origin.y
                    && point.y < frame.origin.y + frame.size.height
                {
                    let idx = app_idx_range.start + i;
                    if idx < app_idx_range.end {
                        app_idx = Some((idx, button));
                    }
                    break;
                }
            }

            if let Some((app_idx, icon_button)) = app_idx {
                // Provide visual feedback that the app has been picked
                // (it may take a while for the splash screen to appear etc)
                () = msg![env; icon_button setAlpha:(0.5 as CGFloat)];
                // Redraw screen, even if this makes the next frame early
                // (the app picker will never be redrawn after this).
                crate::frameworks::core_animation::recomposite_if_necessary(
                    env, /* force: */ true,
                );
                // Ensure touchHLE is responsive from the OS perspective,
                // otherwise screen redraw might not show up? (Unclear if
                // this explanation is correct.)
                run_run_loop_single_iteration(env, main_run_loop);

                let app_path = &apps.as_ref().unwrap()[app_idx].path;
                echo!("Picked: {}", app_path.display());
                break app_path.clone();
            }
            continue;
        }

        if std::mem::take(&mut host_obj.copyright_show) {
            copyright_info_page_idx = 0;
            change_copyright_page(
                env,
                &mut copyright_info_stuff,
                &copyright_info_text,
                copyright_info_page_idx,
            );
            () = msg![env; (copyright_info_stuff.main_view) setHidden:false];
        } else if std::mem::take(&mut host_obj.copyright_hide) {
            () = msg![env; (copyright_info_stuff.main_view) setHidden:true];
        } else if std::mem::take(&mut host_obj.copyright_prev) && copyright_info_page_idx != 0 {
            copyright_info_page_idx -= 1;
            change_copyright_page(
                env,
                &mut copyright_info_stuff,
                &copyright_info_text,
                copyright_info_page_idx,
            );
        } else if std::mem::take(&mut host_obj.copyright_next)
            && Some(copyright_info_page_idx) != copyright_info_stuff.last_page_idx
        {
            copyright_info_page_idx += 1;
            change_copyright_page(
                env,
                &mut copyright_info_stuff,
                &copyright_info_text,
                copyright_info_page_idx,
            );
        } else if std::mem::take(&mut host_obj.quick_options_show) {
            () = msg![env; (quick_options_stuff.main_view) setHidden:false];
        } else if std::mem::take(&mut host_obj.quick_options_hide) {
            () = msg![env; (quick_options_stuff.main_view) setHidden:true];
        } else if std::mem::take(&mut host_obj.scale_hack_default) {
            quick_options_scale_hack = None;
            update_scale_hack_buttons(
                env,
                &quick_options_stuff.scale_hack_buttons,
                quick_options_scale_hack,
            );
        } else if std::mem::take(&mut host_obj.scale_hack1) {
            quick_options_scale_hack = Some(NonZeroU32::new(1).unwrap());
            update_scale_hack_buttons(
                env,
                &quick_options_stuff.scale_hack_buttons,
                quick_options_scale_hack,
            );
        } else if std::mem::take(&mut host_obj.scale_hack2) {
            quick_options_scale_hack = Some(NonZeroU32::new(2).unwrap());
            update_scale_hack_buttons(
                env,
                &quick_options_stuff.scale_hack_buttons,
                quick_options_scale_hack,
            );
        } else if std::mem::take(&mut host_obj.scale_hack3) {
            quick_options_scale_hack = Some(NonZeroU32::new(3).unwrap());
            update_scale_hack_buttons(
                env,
                &quick_options_stuff.scale_hack_buttons,
                quick_options_scale_hack,
            );
        } else if std::mem::take(&mut host_obj.scale_hack4) {
            quick_options_scale_hack = Some(NonZeroU32::new(4).unwrap());
            update_scale_hack_buttons(
                env,
                &quick_options_stuff.scale_hack_buttons,
                quick_options_scale_hack,
            );
        } else if std::mem::take(&mut host_obj.orientation_default) {
            quick_options_orientation = None;
            update_orientation_buttons(
                env,
                &quick_options_stuff.orientation_buttons,
                quick_options_orientation,
            );
        } else if std::mem::take(&mut host_obj.orientation_portrait_upside_down) {
            quick_options_orientation = Some(DeviceOrientation::PortraitUpsideDown);
            update_orientation_buttons(
                env,
                &quick_options_stuff.orientation_buttons,
                quick_options_orientation,
            );
        } else if std::mem::take(&mut host_obj.orientation_landscape_left) {
            quick_options_orientation = Some(DeviceOrientation::LandscapeLeft);
            update_orientation_buttons(
                env,
                &quick_options_stuff.orientation_buttons,
                quick_options_orientation,
            );
        } else if std::mem::take(&mut host_obj.orientation_landscape_right) {
            quick_options_orientation = Some(DeviceOrientation::LandscapeRight);
            update_orientation_buttons(
                env,
                &quick_options_stuff.orientation_buttons,
                quick_options_orientation,
            );
        } else if let Some(enabled) = std::mem::take(&mut host_obj.analog_stick_tilt_controls) {
            quick_options_analog_stick_tilt_controls = enabled;
        } else if let Some(enabled) = std::mem::take(&mut host_obj.network) {
            quick_options_network = enabled;
        } else if let Some(fullscreen) = std::mem::take(&mut host_obj.fullscreen) {
            quick_options_fullscreen = match fullscreen {
                false => None,
                true => Some(()),
            };
        }
    };

    // Apply user-specified overrides
    if let Some(scale_hack) = quick_options_scale_hack {
        option_args.push(format!("--scale-hack={}", scale_hack.get()));
    }
    if let Some(orientation) = quick_options_orientation {
        option_args.push(
            match orientation {
                DeviceOrientation::LandscapeLeft => "--landscape-left",
                DeviceOrientation::LandscapeRight => "--landscape-right",
                DeviceOrientation::PortraitUpsideDown => "--upside-down",
                _ => todo!(),
            }
            .to_string(),
        );
    }
    if let Some(()) = quick_options_fullscreen {
        option_args.push("--fullscreen".to_string());
    }
    if !quick_options_analog_stick_tilt_controls {
        option_args.push("--disable-analog-stick-tilt-controls".to_string());
    }
    if quick_options_network {
        option_args.push("--allow-network-access".to_string());
    }

    // Return the environment so some parts of it can be salvaged.
    (app_path, option_args)
}

const ICON_SIZE: CGSize = CGSize {
    width: 57.0,
    height: 57.0,
};

struct IconGridStuff {
    page_control: id,
    icon_buttons_and_labels: Vec<(id, id)>,
    placeholder_icon: Option<id>,
    pages: Vec<std::ops::Range<usize>>,
}

fn make_icon_grid(
    env: &mut Environment,
    delegate: id,
    main_view: id,
    app_frame: CGRect,
    total_app_count: usize,
    have_wallpaper: bool,
) -> IconGridStuff {
    let num_cols = 4;
    let num_cols_f = num_cols as CGFloat;
    let num_rows = 4;
    let label_size = CGSize {
        width: 74.0,
        height: 13.0,
    };
    let icon_gap_x: CGFloat = 19.0;
    let icon_gap_y: CGFloat = 4.0 + label_size.height + 8.0;
    let icon_grid_width = (ICON_SIZE.width * num_cols_f) + icon_gap_x * (num_cols_f - 1.0);
    let icon_grid_origin = CGPoint {
        x: (app_frame.size.width - icon_grid_width) / 2.0,
        y: 8.0,
    };

    let grid_view_frame = CGRect {
        origin: CGPoint { x: 0.0, y: 0.0 },
        size: CGSize {
            width: app_frame.size.width,
            height: app_frame.size.height - 100.0, // divider
        },
    };
    let grid_view: id = msg_class![env; _touchHLE_AppPickerGridView alloc];
    let grid_view: id = msg![env; grid_view initWithFrame:grid_view_frame];
    {
        let host_obj = env
            .objc
            .borrow_mut::<AppPickerGridViewHostObject>(grid_view);
        host_obj.delegate = delegate;
    }
    () = msg![env; main_view addSubview:grid_view];

    let page_control: id = msg_class![env; _touchHLE_AppPickerPageControl alloc];
    let page_control_frame = CGRect {
        origin: CGPoint {
            x: 0.0,
            // The dots stay where they were, 10 points above the grid's
            // bottom, but the strip is taller, to be easier to tap.
            y: grid_view_frame.size.height - 26.0,
        },
        size: CGSize {
            width: app_frame.size.width,
            height: 32.0,
        },
    };
    let page_control: id = msg![env; page_control initWithFrame:page_control_frame];
    () = msg![env; page_control setDelegate:delegate];
    () = msg![env; main_view addSubview:page_control];

    let mut icon_buttons_and_labels = Vec::new();

    for i in 0..(num_cols * num_rows) {
        let col = i % num_cols;
        let row = i / num_cols;

        // Rounding is needed here to avoid a blurry or offset image.
        let icon_frame = CGRect {
            origin: CGPoint {
                x: (icon_grid_origin.x + (col as CGFloat) * (ICON_SIZE.width + icon_gap_x)).round(),
                y: (icon_grid_origin.y + (row as CGFloat) * (ICON_SIZE.height + icon_gap_y))
                    .round(),
            },
            size: ICON_SIZE,
        };
        let icon_button: id = msg_class![env; UIButton buttonWithType:UIButtonTypeCustom];
        () = msg![env; icon_button setFrame:icon_frame];
        let image_view: id = msg![env; icon_button imageView];
        let bounds: CGRect = msg![env; icon_button bounds];
        () = msg![env; image_view setFrame:bounds];
        () = msg![env; icon_button setUserInteractionEnabled:false];
        () = msg![env; grid_view addSubview:icon_button];

        // Rounding is needed here to avoid blurry text.
        let label_frame = CGRect {
            origin: CGPoint {
                x: (icon_frame.origin.x - (label_size.width - ICON_SIZE.width) / 2.0).round(),
                y: (icon_frame.origin.y + ICON_SIZE.height + 4.0).round(),
            },
            size: label_size,
        };
        let label: id = msg_class![env; UILabel alloc];
        let label: id = msg![env; label initWithFrame:label_frame];
        () = msg![env; label setTextAlignment:UITextAlignmentCenter];
        let font_size: CGFloat = label_size.height - 2.0;
        let font: id = if have_wallpaper {
            msg_class![env; UIFont systemFontOfSize:font_size]
        } else {
            msg_class![env; UIFont boldSystemFontOfSize:font_size]
        };
        () = msg![env; label setFont:font];
        let text_color: id = if have_wallpaper {
            msg_class![env; UIColor whiteColor]
        } else {
            msg_class![env; UIColor lightGrayColor]
        };
        () = msg![env; label setTextColor:text_color];
        let bg_color: id = msg_class![env; UIColor clearColor];
        () = msg![env; label setBackgroundColor:bg_color];
        () = msg![env; grid_view addSubview:label];

        icon_buttons_and_labels.push((icon_button, label));

        {
            let host_obj = env
                .objc
                .borrow_mut::<AppPickerGridViewHostObject>(grid_view);
            host_obj.icon_buttons.push(icon_button);
        }
    }

    // TODO: Use UIScrollView pagination and UIPageControl once available.
    let mut pages = Vec::new();
    let mut start = 0;
    while start < total_app_count {
        let end = (start + icon_buttons_and_labels.len()).min(total_app_count);
        pages.push(start..end);
        start = end;
    }

    let page_count = pages.len();
    () = msg![env; page_control setNumberOfPages:(page_count as NSUInteger)];

    IconGridStuff {
        page_control,
        icon_buttons_and_labels,
        placeholder_icon: None,
        pages,
    }
}

fn make_icon_from_glyph(
    env: &mut Environment,
    glyph: char,
    font_size: CGFloat,
    baseline_offset: CGFloat,
    bg_color: (CGFloat, CGFloat, CGFloat, CGFloat),
) -> id {
    let color_space = CGColorSpaceCreateDeviceRGB(env);
    let context = CGBitmapContextCreate(
        env,
        Ptr::null(),
        ICON_SIZE.width as u32,
        ICON_SIZE.height as u32,
        8,
        4 * (ICON_SIZE.width as u32),
        color_space,
        kCGImageAlphaPremultipliedLast,
    );
    UIGraphicsPushContext(env, context);

    // Compensate for row order inversion
    CGContextTranslateCTM(env, context, 0.0, ICON_SIZE.height);
    CGContextScaleCTM(env, context, 1.0, -1.0);

    let (r, g, b, a) = bg_color;
    CGContextSetRGBFillColor(env, context, r, g, b, a);
    CGContextFillRect(
        env,
        context,
        CGRect {
            origin: CGPoint { x: 0.0, y: 0.0 },
            size: ICON_SIZE,
        },
    );

    let font: id = msg_class![env; UIFont systemFontOfSize:font_size];
    let glyph_string: id = ns_string::from_rust_string(env, [glyph].into_iter().collect());
    let glyph_size: CGSize = msg![env; glyph_string sizeWithFont:font];
    CGContextSetRGBFillColor(env, context, 1.0, 1.0, 1.0, 1.0); // white
    let glyph_origin = CGPoint {
        x: ICON_SIZE.width / 2.0 - glyph_size.width / 2.0,
        y: ICON_SIZE.height / 2.0 - glyph_size.height / 2.0 + baseline_offset,
    };
    let _: CGSize = msg![env; glyph_string drawAtPoint:glyph_origin withFont:font];
    release(env, glyph_string);

    UIGraphicsPopContext(env);

    let cg_image = CGBitmapContextCreateImage(env, context);
    // This radius should match the one in src/bundle.rs.
    cg_image::borrow_image_mut(&mut env.objc, cg_image).round_corners(
        (10.0 / 57.0) * ICON_SIZE.width,
        /* four_corners: */ true,
        /* add_sheen: */ true,
    );
    CGContextRelease(env, context);

    let ui_image: id = msg_class![env; UIImage imageWithCGImage:cg_image];
    release(env, cg_image);

    ui_image
}

fn update_icon_grid(
    env: &mut Environment,
    icon_grid_stuff: &mut IconGridStuff,
    apps: &mut [AppInfo],
    page_idx: usize,
) {
    let app_idx_range = icon_grid_stuff.pages[page_idx].clone();

    let mut icon_iter = icon_grid_stuff.icon_buttons_and_labels.iter();

    for app_idx in app_idx_range.clone() {
        let app = &mut apps[app_idx];

        let &(icon_button, label) = icon_iter.next().unwrap();

        if let Some(icon) = app.icon.take() {
            let image = cg_image::from_image(env, icon);
            let image: id = msg_class![env; UIImage imageWithCGImage:image];
            app.icon_ui_image = Some(image);
        }

        let image = app.icon_ui_image.unwrap_or_else(|| {
            *icon_grid_stuff.placeholder_icon.get_or_insert_with(|| {
                make_icon_from_glyph(env, '?', 40.0, 0.0, (0.5, 0.5, 0.5, 1.0))
            })
        });
        () = msg![env; icon_button setImage:image forState:UIControlStateNormal];

        let text = *app
            .display_name_ns_string
            .get_or_insert_with(|| ns_string::from_rust_string(env, app.display_name.clone()));
        () = msg![env; label setText:text];
    }

    // There may be remaining spaces might need to be blanked.
    for &(icon_button, label) in icon_iter {
        () = msg![env; icon_button setImage:nil forState:UIControlStateNormal];
        () = msg![env; label setText:(ns_string::get_static_str(env, ""))];
    }
}

fn make_button_row(
    env: &mut Environment,
    delegate: id,
    super_view: id,
    super_view_size: CGSize,
    buttons_row_center: CGFloat,
    buttons: &[(&'static str, &'static str)],
    font_size: Option<CGFloat>,
) -> Vec<id> {
    let margin = 10.0;

    let button_size = CGSize {
        width: (super_view_size.width - margin) / (buttons.len() as CGFloat) - margin,
        height: 30.0,
    };
    let mut button_frame = CGRect {
        origin: CGPoint {
            x: margin,
            y: buttons_row_center - button_size.height / 2.0,
        },
        size: button_size,
    };

    let mut ui_buttons = Vec::new();
    for (title_text, selector) in buttons {
        let button: id = msg_class![env; UIButton buttonWithType:UIButtonTypeRoundedRect];
        let text = ns_string::get_static_str(env, title_text);
        () = msg![env; button setTitle:text forState:UIControlStateNormal];
        () = msg![env; button setFrame:button_frame];
        // FIXME: manually calling layoutSubviews shouldn't be needed?
        () = msg![env; button layoutSubviews];

        if let Some(font_size) = font_size {
            let label: id = msg![env; button titleLabel];
            let font: id = msg_class![env; UIFont systemFontOfSize:font_size];
            () = msg![env; label setFont:font];
        }

        let selector = env.objc.lookup_selector(selector).unwrap();
        () = msg![env; button addTarget:delegate
                                 action:selector
                       forControlEvents:UIControlEventTouchUpInside];
        () = msg![env; super_view addSubview:button];

        button_frame.origin.x += button_size.width + margin;
        ui_buttons.push(button);
    }
    ui_buttons
}

struct CopyrightInfoStuff {
    main_view: id,
    text_frame: CGRect,
    text_label: id,
    font: id,
    pages: Vec<(std::ops::Range<usize>, CGFloat)>,
    last_page_idx: Option<usize>,
    prev_page_button: id,
    next_page_button: id,
}

fn setup_copyright_info(
    env: &mut Environment,
    delegate: id,
    super_view: id,
    app_frame: CGRect,
) -> CopyrightInfoStuff {
    let main_frame = CGRect {
        origin: CGPoint { x: 0.0, y: 0.0 },
        size: app_frame.size,
    };

    let divider = main_frame.size.height - 40.0;

    // Container for all the other stuff

    let main_view: id = msg_class![env; UIView alloc];
    let main_view: id = msg![env; main_view initWithFrame:main_frame];
    // TODO: Isn't white the default?
    let bg_color: id = msg_class![env; UIColor whiteColor];
    () = msg![env; main_view setBackgroundColor:bg_color];
    // This main_view is hidden until the copyright info button is tapped.
    () = msg![env; main_view setHidden:true];
    () = msg![env; super_view addSubview:main_view];

    // UILabel that will display part of the copyright text

    let padding = 10.0;
    let text_frame = CGRect {
        origin: CGPoint {
            x: padding,
            y: padding,
        },
        size: CGSize {
            width: app_frame.size.width - padding * 2.0,
            height: divider - padding * 2.0,
        },
    };

    let text_label: id = msg_class![env; UILabel alloc];
    let text_label: id = msg![env; text_label initWithFrame:text_frame];
    () = msg![env; text_label setNumberOfLines:0]; // unlimited
    let text_color: id = msg_class![env; UIColor blackColor];
    () = msg![env; text_label setTextColor:text_color];
    let bg_color: id = msg_class![env; UIColor clearColor];
    () = msg![env; text_label setBackgroundColor:bg_color];
    let font_size: CGFloat = 16.0;
    let font: id = msg_class![env; UIFont systemFontOfSize:font_size];
    () = msg![env; text_label setFont:font];
    () = msg![env; main_view addSubview:text_label];

    // Navigation

    let buttons_row_center = (main_frame.size.height + divider) / 2.0;
    let buttons = make_button_row(
        env,
        delegate,
        main_view,
        main_frame.size,
        buttons_row_center,
        &[
            ("↑", "copyrightInfoPrevPage"),
            ("↓", "copyrightInfoNextPage"),
            ("×", "copyrightInfoHide"),
        ],
        Some(30.0),
    );

    CopyrightInfoStuff {
        main_view,
        text_frame,
        text_label,
        font,
        pages: Vec::new(),
        last_page_idx: None,
        prev_page_button: buttons[0],
        next_page_button: buttons[1],
    }
}

fn change_copyright_page(
    env: &mut Environment,
    copyright_info_stuff: &mut CopyrightInfoStuff,
    copyright_info_text: &str,
    page_idx: usize,
) {
    // TODO: Eventually this should be ripped out and replaced with a scrolling
    // UITextView, once that's implemented.

    let &mut CopyrightInfoStuff {
        text_frame,
        text_label,
        font,
        ref mut pages,
        ref mut last_page_idx,
        prev_page_button,
        next_page_button,
        ..
    } = copyright_info_stuff;

    // Lazily lay out pages of text as needed.

    if page_idx == pages.len() {
        let mut page_start = pages.last().map_or(0, |page| page.0.end);
        while copyright_info_text[page_start..].starts_with([' ', '\n', '\r']) {
            page_start += 1;
        }
        let mut page_height = 0.0;
        let page_end = loop {
            let mut line_start = page_start;
            while line_start < copyright_info_text.len() {
                let is_first_line = line_start == page_start;

                let line_end = if let Some(i) = copyright_info_text[line_start..].find('\n') {
                    line_start + i + 1
                } else {
                    copyright_info_text.len()
                };

                let line = &copyright_info_text[line_start..line_end];

                // Force pagination before Markdown-style headings, if any
                // appear in the license text
                if !is_first_line && line.starts_with("###") {
                    break;
                }

                let line_temp = ns_string::from_rust_string(env, line.to_string());
                let line_size: CGSize = msg![env; line_temp sizeWithFont:font
                                                       constrainedToSize:(text_frame.size)];
                // Avoid accumulation of old line strings.
                release(env, line_temp);

                if page_height + line_size.height > text_frame.size.height {
                    break;
                }

                page_height += line_size.height;
                line_start = line_end;

                // Force pagination after dividers
                if !is_first_line && line.starts_with("---") {
                    break;
                }
            }
            let page_end = line_start;
            assert!(page_start != page_end);

            // Avoid entirely blank pages
            if copyright_info_text[page_start..page_end].trim() == "" {
                page_start = page_end;
            } else {
                break page_end;
            }
        };
        assert!(page_start != page_end);
        pages.push((page_start..page_end, page_height));
        if page_end == copyright_info_text.len() {
            *last_page_idx = Some(page_idx);
        }
    }

    // Actually display the page

    let (page, page_height) = pages[page_idx].clone();
    let page = &copyright_info_text[page];

    let page: id = ns_string::from_rust_string(env, page.to_string());
    () = msg![env; text_label setText:page];
    // Avoid accumulation of old page strings.
    release(env, page);

    // UILabel always vertically centers text. Work around that by resizing it.
    let label_frame = CGRect {
        origin: text_frame.origin,
        size: CGSize {
            width: text_frame.size.width,
            // The page height is slightly off, a little padding is needed.
            height: page_height + 10.0,
        },
    };
    () = msg![env; text_label setFrame:label_frame];

    () = msg![env; prev_page_button setHidden:(page_idx == 0)];
    () = msg![env; next_page_button setHidden:(Some(page_idx) == *last_page_idx)];
}

struct QuickOptionsStuff {
    main_view: id,
    scale_hack_buttons: [id; 5],
    orientation_buttons: [id; 4],
}

fn setup_quick_options(
    env: &mut Environment,
    delegate: id,
    super_view: id,
    app_frame: CGRect,
) -> QuickOptionsStuff {
    // UIView*
    let main_frame = CGRect {
        origin: CGPoint { x: 0.0, y: 0.0 },
        size: app_frame.size,
    };

    // Container for all the other stuff

    let main_view: id = msg_class![env; UIView alloc];
    let main_view: id = msg![env; main_view initWithFrame:main_frame];
    // TODO: Isn't white the default?
    let bg_color: id = msg_class![env; UIColor whiteColor];
    () = msg![env; main_view setBackgroundColor:bg_color];
    // This main_view is hidden until the copyright info button is tapped.
    () = msg![env; main_view setHidden:true];
    () = msg![env; super_view addSubview:main_view];

    let divider = 40.0;

    // Close button
    {
        let button_frame = CGRect {
            origin: CGPoint {
                x: main_frame.size.width - 30.0,
                y: 10.0,
            },
            size: CGSize {
                width: 20.0,
                height: 20.0,
            },
        };

        let button: id = msg_class![env; UIButton buttonWithType:UIButtonTypeRoundedRect];
        let text = ns_string::get_static_str(env, "×");
        () = msg![env; button setTitle:text forState:UIControlStateNormal];
        () = msg![env; button setFrame:button_frame];
        // FIXME: manually calling layoutSubviews shouldn't be needed?
        () = msg![env; button layoutSubviews];

        let label: id = msg![env; button titleLabel];
        let font: id = msg_class![env; UIFont systemFontOfSize:(30.0 as CGFloat)];
        () = msg![env; label setFont:font];

        let selector = env.objc.lookup_selector("quickOptionsHide").unwrap();
        () = msg![env; button addTarget:delegate
                                 action:selector
                       forControlEvents:UIControlEventTouchUpInside];
        () = msg![env; main_view addSubview:button];
    }

    enum RowKind {
        Label(&'static str),
        Buttons(&'static [(&'static str, &'static str)]),
        Switch(&'static str, bool),
    }
    let rows = [
        RowKind::Label("Scale hack"),
        RowKind::Buttons(&[
            ("Default", "scaleHackDefault"),
            ("Off", "scaleHack1"),
            ("2×", "scaleHack2"),
            ("3×", "scaleHack3"),
            ("4×", "scaleHack4"),
        ]),
        RowKind::Label("Orientation"),
        RowKind::Buttons(&[
            ("Default", "orientationDefault"),
            ("←", "orientationLandscapeLeft"),
            ("→", "orientationLandscapeRight"),
            ("↓", "orientationPortraitUpsideDown"),
        ]),
        RowKind::Label("Network access"),
        RowKind::Switch("network:", false),
        RowKind::Label("Use analog sticks for tilt controls"),
        RowKind::Switch("analogStickTiltControls:", true),
        // ---- (divider for stuff skipped below)
        RowKind::Label("Fullscreen (override)"),
        RowKind::Switch("fullscreen:", false),
    ];
    let rows_len_full = rows.len();
    let rows = if crate::window::Window::rotatable_fullscreen() {
        // Fullscreen option doesn't make sense on always-fullscreen platforms
        &rows[..rows.len() - 2]
    } else {
        &rows[..]
    };

    let mut button_rows = Vec::new();
    for (i, row) in rows.iter().enumerate() {
        let row_center = divider
            + ((1 + i) as CGFloat)
                * ((main_frame.size.height - divider) / ((rows_len_full + 1) as CGFloat));

        match *row {
            RowKind::Label(text) => {
                let frame = CGRect {
                    origin: CGPoint {
                        x: 0.0,
                        y: row_center - 30.0 / 2.0,
                    },
                    size: CGSize {
                        width: main_frame.size.width,
                        height: 30.0,
                    },
                };

                let label: id = msg_class![env; UILabel alloc];
                let label: id = msg![env; label initWithFrame:frame];
                let text = ns_string::get_static_str(env, text);
                () = msg![env; label setText:text];
                () = msg![env; label setTextAlignment:UITextAlignmentCenter];
                () = msg![env; main_view addSubview:label];
            }
            RowKind::Buttons(buttons) => {
                button_rows.push(make_button_row(
                    env,
                    delegate,
                    main_view,
                    main_frame.size,
                    row_center,
                    buttons,
                    /* font_size: */ None,
                ));
            }
            RowKind::Switch(selector, default_state) => {
                let switch_frame = CGRect {
                    origin: CGPoint {
                        x: main_frame.size.width / 2.0 - 94.0 / 2.0,
                        y: row_center - 27.0 / 2.0,
                    },
                    size: Default::default(),
                };

                let switch: id = msg_class![env; UISwitch alloc];
                let switch: id = msg![env; switch initWithFrame:switch_frame];
                () = msg![env; switch setOn:default_state];
                let selector = env.objc.lookup_selector(selector).unwrap();
                () = msg![env; switch addTarget:delegate
                                         action:selector
                               forControlEvents:UIControlEventValueChanged];
                () = msg![env; main_view addSubview:switch];
            }
        }
    }

    QuickOptionsStuff {
        main_view,
        scale_hack_buttons: button_rows[0][..].try_into().unwrap(),
        orientation_buttons: button_rows[1][..].try_into().unwrap(),
    }
}
