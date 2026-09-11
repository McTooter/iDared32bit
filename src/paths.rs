/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Paths for host files used by touchHLE: settings, fonts, etc.
//!
//! There are three categories of files:
//!
//! * Resources bundled with touchHLE that neither touchHLE nor the user should
//!   modify: [DYLIBS_DIR], [FONTS_DIR], [DEFAULT_OPTIONS_FILE]. Depending on
//!   the platform these may or may not be ordinary files, and must be accessed
//!   through [ResourceFile].
//! * Files the user is expected to modify, but not touchHLE: [APPS_DIR],
//!   [USER_OPTIONS_FILE], [WALLPAPER_FILES]. These are ordinary files and are
//!   found in [user_data_base_path].
//! * Files that touchHLE will create and modify, and the user may modify if
//!   they want to: [SANDBOX_DIR], [PHOTO_ALBUM_DIR].
//!   These are ordinary files and are found in [user_data_base_path].
//!
//! See also [crate::fs], which provides a virtual filesystem for the guest app
//! and defines path types.

use std::borrow::Cow;
use std::io::{Read, Seek};
use std::path::{Path, PathBuf};

/// Name of the directory containing ARMv6 dynamic libraries bundled with
/// touchHLE.
pub const DYLIBS_DIR: &str = "touchHLE_dylibs";

/// Name of the directory containing fonts bundled with touchHLE.
pub const FONTS_DIR: &str = "touchHLE_fonts";

/// Name of the file containing touchHLE's default options for various apps.
pub const DEFAULT_OPTIONS_FILE: &str = "touchHLE_default_options.txt";

/// macOS-only: If touchHLE is located in a .app bundle, return the path of the
/// Resources directory. If touchHLE is not located in a .app bundle, return
/// [None].
#[allow(dead_code)]
fn get_macos_bundled_resources_path() -> Option<PathBuf> {
    if std::env::consts::OS != "macos" {
        return None;
    }
    let base_path = PathBuf::from(sdl2::filesystem::base_path().ok()?);
    if base_path.file_name().is_some_and(|p| p == "Resources") {
        Some(base_path)
    } else {
        None
    }
}

/// Abstraction over a platform-specific type for accessing a resource bundled
/// with touchHLE.
pub struct ResourceFile {
    inner: ResourceFileInner,
}
enum ResourceFileInner {
    #[cfg(target_os = "android")]
    Asset(sdl2::rwops::RWops<'static>),
    // Embedded, not shipped loose: Apple's validator rejects any Mach-O
    // file it finds in the bundle outside the app's own executable.
    #[cfg(target_os = "ios")]
    Embedded(std::io::Cursor<&'static [u8]>),
    #[cfg(not(target_os = "android"))]
    Disk(std::fs::File),
}
impl Read for ResourceFileInner {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        match self {
            #[cfg(target_os = "android")]
            Self::Asset(f) => f.read(buf),
            #[cfg(target_os = "ios")]
            Self::Embedded(c) => c.read(buf),
            #[cfg(not(target_os = "android"))]
            Self::Disk(f) => f.read(buf),
        }
    }
}
impl Seek for ResourceFileInner {
    fn seek(&mut self, pos: std::io::SeekFrom) -> std::io::Result<u64> {
        match self {
            #[cfg(target_os = "android")]
            Self::Asset(f) => f.seek(pos),
            #[cfg(target_os = "ios")]
            Self::Embedded(c) => c.seek(pos),
            #[cfg(not(target_os = "android"))]
            Self::Disk(f) => f.seek(pos),
        }
    }
}
#[cfg(target_os = "ios")]
fn embedded_dylib_bytes(path: &str) -> Option<&'static [u8]> {
    match path {
        "touchHLE_dylibs/libgcc_s.1.dylib" => Some(
            include_bytes!(concat!(
                env!("CARGO_MANIFEST_DIR"),
                "/touchHLE_dylibs/libgcc_s.1.dylib"
            ))
            .as_slice(),
        ),
        "touchHLE_dylibs/libsqlite3.dylib" => Some(
            include_bytes!(concat!(
                env!("CARGO_MANIFEST_DIR"),
                "/touchHLE_dylibs/libsqlite3.dylib"
            ))
            .as_slice(),
        ),
        "touchHLE_dylibs/libstdc++.6.0.9.dylib" => Some(
            include_bytes!(concat!(
                env!("CARGO_MANIFEST_DIR"),
                "/touchHLE_dylibs/libstdc++.6.0.9.dylib"
            ))
            .as_slice(),
        ),
        "touchHLE_dylibs/libxml2.2.dylib" => Some(
            include_bytes!(concat!(
                env!("CARGO_MANIFEST_DIR"),
                "/touchHLE_dylibs/libxml2.2.dylib"
            ))
            .as_slice(),
        ),
        "touchHLE_dylibs/libz.1.2.3.dylib" => Some(
            include_bytes!(concat!(
                env!("CARGO_MANIFEST_DIR"),
                "/touchHLE_dylibs/libz.1.2.3.dylib"
            ))
            .as_slice(),
        ),
        _ => None,
    }
}
impl ResourceFile {
    pub fn open(path: &str) -> Result<Self, String> {
        #[cfg(target_os = "ios")]
        if let Some(bytes) = embedded_dylib_bytes(path) {
            return Ok(Self {
                inner: ResourceFileInner::Embedded(std::io::Cursor::new(bytes)),
            });
        }

        Ok(Self {
            inner: {
                // On Android, these resources are included as "assets" within
                // the APK. We access them via SDL2's wrapper of Android's
                // assets API.
                #[cfg(target_os = "android")]
                {
                    ResourceFileInner::Asset(sdl2::rwops::RWops::from_file(path, "r")?)
                }

                // On other OSes (and for non-embedded resources on iOS),
                // resources are accessed as ordinary files.
                #[cfg(not(target_os = "android"))]
                {
                    let base_path = get_macos_bundled_resources_path();
                    // When not in a bundle, look in the current directory.
                    let path = base_path.as_deref().unwrap_or(Path::new(".")).join(path);
                    ResourceFileInner::Disk(std::fs::File::open(path).map_err(|e| e.to_string())?)
                }
            },
        })
    }
    pub fn get(&mut self) -> &mut (impl Read + Seek) {
        &mut self.inner
    }
}
impl std::fmt::Debug for ResourceFile {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> Result<(), std::fmt::Error> {
        write!(f, "ResourceFile")
    }
}

/// Whether various resources are in user-accessible files. If they aren't,
/// touchHLE has to be able to display their license terms.
pub const RESOURCES_ARE_EXTERNAL_FILES: bool = cfg!(not(target_os = "android"));

/// Name of the directory where the user can put apps if they want them to
/// appear in the app picker.
pub const APPS_DIR: &str = "touchHLE_apps";

/// Name of the file intended for the user's own options.
pub const USER_OPTIONS_FILE: &str = "touchHLE_options.txt";

/// Names of files the user can put a wallpaper image (for the app picker) in.
#[allow(unused)]
pub const WALLPAPER_FILES: &[&str] = &[
    "touchHLE_wallpaper.png",
    "touchHLE_wallpaper.jpg",
    "touchHLE_wallpaper.jpeg",
];

/// Name of the directory where touchHLE will store sandboxed app data, e.g.
/// the `Documents` directory.
pub const SANDBOX_DIR: &str = "touchHLE_sandbox";

/// Name of the directory where touchHLE will store IMG_####.PNG files saved to
/// the Photo Album.
pub const PHOTO_ALBUM_DIR: &str = "DCIM/100APPLE";

/// Get a platform-specific base path needed for accessing touchHLE's
/// user-modifiable files. This is empty on platforms other than Android.
pub fn user_data_base_path() -> Cow<'static, Path> {
    #[cfg(target_os = "android")]
    unsafe {
        // This is an exception to the rule that SDL2 should only be used
        // directly from src/window.rs. This is just too distant from windowing
        // to belong there.

        // Android storage has evolved in a quite messy fashion. Both "internal
        // storage" and "external storage" (aka the "SD card") are likely to be
        // internal on a modern device, as absurd as that might sound. SDL2 has
        // APIs to get paths for both. We use the "external storage" because
        // it's more likely to be user-accessible.
        extern "C" {
            fn SDL_AndroidGetExternalStoragePath() -> *const std::ffi::c_char;
        }
        let path = SDL_AndroidGetExternalStoragePath();
        if path.is_null() {
            log!("Couldn't get Android external storage path!");
            panic!();
        }
        Cow::from(Path::new(std::ffi::CStr::from_ptr(path).to_str().unwrap()))
    }
    #[cfg(not(any(target_os = "android", target_os = "ios")))]
    {
        // When touchHLE is run from a .app bundle on macOS, the user might not
        // be able to control the current directory, so user data needs to go in
        // a standard location.
        if get_macos_bundled_resources_path().is_some() {
            return Cow::from(PathBuf::from(
                sdl2::filesystem::pref_path("touchhle.org", "touchHLE").unwrap(),
            ));
        }
        Cow::from(Path::new("."))
    }

    #[cfg(target_os = "ios")]
    {
        let mut pref_path =
            PathBuf::from(sdl2::filesystem::pref_path("touchhle.org", "touchHLE").unwrap());

        // touchHLE
        pref_path.pop();
        // touchhle.org
        pref_path.pop();
        // Application Support
        pref_path.pop();
        // Library
        pref_path.pop();

        pref_path.push("Documents");

        return Cow::from(pref_path);
    }
}

/// Get a URI that can be used to open a file manager or similar for the path
/// that [user_data_base_path] represents.
pub fn url_for_opening_user_data_dir() -> Result<String, String> {
    if std::env::consts::OS == "android" {
        // See DocumentsProvider.kt, app/build.gradle and AndroidManifest.xml
        let brand = crate::branding();
        Ok(format!(
            "content://org.touchhle.android{}{}.provider/root/root",
            if brand.is_empty() { "" } else { "." },
            brand.to_lowercase()
        ))
    } else {
        let path_buf;
        let path = if cfg!(target_os = "ios") {
            user_data_base_path()
        } else {
            path_buf = user_data_base_path()
                .join(".")
                .canonicalize()
                .map_err(|e| format!("Can't canonicalize path to user data directory: {e}"))?;
            Cow::from(path_buf)
        };

        let path_str = path
            .to_str()
            .ok_or_else(|| "User data directory path is not UTF-8".to_string())?;

        if cfg!(target_os = "ios") {
            Ok(format!("shareddocuments://{path_str}"))
        } else {
            // std::fs::canonicalize() on Windows uses the extended-length path
            // syntax, but Windows Explorer doesn't understand it.
            let path_str = if std::env::consts::OS == "windows" {
                path_str.strip_prefix("\\\\?\\").unwrap_or(path_str)
            } else {
                path_str
            };
            Ok(format!("file://{path_str}"))
        }
    }
}

/// Only meaningful on certain OSes: create the user data directory if it
/// doesn't exist, and populate it with templates or README files. (On other
/// platforms these are simply bundled with touchHLE in a ZIP file.)
pub fn prepopulate_user_data_dir() {
    if std::env::consts::OS != "android"
        && std::env::consts::OS != "macos"
        && std::env::consts::OS != "ios"
    {
        return;
    }
    let base_path = user_data_base_path();
    if base_path == Path::new(".") {
        return;
    }

    let apps_dir = base_path.join(APPS_DIR);
    if !apps_dir.is_dir() {
        match std::fs::create_dir(&apps_dir) {
            Ok(()) => {
                log!("Created: {}", apps_dir.display());
            }
            Err(e) => {
                log!("Warning: Couldn't create {}: {}", apps_dir.display(), e);
            }
        }
    }

    fn create_file(path: &Path, content: &str) {
        match std::fs::write(path, content) {
            Ok(()) => {
                log!("Created: {}", path.display());
            }
            Err(e) => {
                log!("Warning: Couldn't create {}: {}", path.display(), e);
            }
        }
    }

    let apps_dir_readme = apps_dir.join("README.txt");
    if !apps_dir_readme.is_file() {
        let content = include_str!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/touchHLE_apps/README.txt"
        ));
        create_file(&apps_dir_readme, content);
    }

    let user_options = base_path.join(USER_OPTIONS_FILE);
    if !user_options.is_file() {
        let content = include_str!(concat!(env!("CARGO_MANIFEST_DIR"), "/touchHLE_options.txt"));
        create_file(&user_options, content);
    }

    let options_help = base_path.join("OPTIONS_HELP.txt");
    if !options_help.is_file() {
        create_file(&options_help, crate::options::OPTIONS_HELP);
    }
}
