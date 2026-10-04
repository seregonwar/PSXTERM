//! Compile-only backend selection check. No entry point; no target execution.
#![no_std]

#[cfg(not(target_os = "freebsd"))]
compile_error!("inspect entropy imports with the FreeBSD target");

// Ordinary safe APIs, kept visible for object-file inspection. The probe
// does not supply an entropy replacement or claim that a backend works.
#[unsafe(no_mangle)]
pub fn psx_entropy_fill02(buffer: &mut [u8]) -> bool {
    getrandom02::getrandom(buffer).is_ok()
}

#[unsafe(no_mangle)]
pub fn psx_entropy_fill03(buffer: &mut [u8]) -> bool {
    getrandom03::fill(buffer).is_ok()
}

#[unsafe(no_mangle)]
pub fn psx_entropy_fill04(buffer: &mut [u8]) -> bool {
    getrandom04::fill(buffer).is_ok()
}
