//! Compile-only libc layout inventory. Contains no functions or entry point.
#![no_std]

#[cfg(not(target_os = "freebsd"))]
compile_error!("use the FreeBSD target when comparing with the PS5 SDK headers");

use core::mem::{align_of, offset_of, size_of};

macro_rules! value {
    ($name:ident, $expression:expr) => {
        #[unsafe(no_mangle)]
        pub static $name: u64 = ($expression) as u64;
    };
}

value!(psx_abi_pointer_size, size_of::<*const ()>());
value!(psx_abi_time_t_size, size_of::<libc::time_t>());
value!(psx_abi_off_t_size, size_of::<libc::off_t>());
value!(psx_abi_ino_t_size, size_of::<libc::ino_t>());
value!(psx_abi_nlink_t_size, size_of::<libc::nlink_t>());
value!(psx_abi_pthread_t_size, size_of::<libc::pthread_t>());
value!(
    psx_abi_pthread_mutex_t_size,
    size_of::<libc::pthread_mutex_t>()
);
value!(
    psx_abi_pthread_cond_t_size,
    size_of::<libc::pthread_cond_t>()
);
value!(
    psx_abi_pthread_rwlock_t_size,
    size_of::<libc::pthread_rwlock_t>()
);
value!(
    psx_abi_pthread_attr_t_size,
    size_of::<libc::pthread_attr_t>()
);
value!(psx_abi_sigset_t_size, size_of::<libc::sigset_t>());
value!(psx_abi_stat_size, size_of::<libc::stat>());
value!(psx_abi_stat_align, align_of::<libc::stat>());
value!(psx_abi_stat_st_ino_offset, offset_of!(libc::stat, st_ino));
value!(
    psx_abi_stat_st_nlink_offset,
    offset_of!(libc::stat, st_nlink)
);
value!(psx_abi_stat_st_size_offset, offset_of!(libc::stat, st_size));
value!(
    psx_abi_stat_st_atim_offset,
    offset_of!(libc::stat, st_atime)
);
value!(
    psx_abi_stat_st_mtim_offset,
    offset_of!(libc::stat, st_mtime)
);
value!(psx_abi_kevent_size, size_of::<libc::kevent>());
value!(psx_abi_kevent_align, align_of::<libc::kevent>());
value!(psx_abi_kevent_ident_offset, offset_of!(libc::kevent, ident));
value!(psx_abi_kevent_flags_offset, offset_of!(libc::kevent, flags));
value!(psx_abi_kevent_data_offset, offset_of!(libc::kevent, data));
value!(psx_abi_kevent_udata_offset, offset_of!(libc::kevent, udata));
value!(psx_abi_timespec_size, size_of::<libc::timespec>());
value!(
    psx_abi_timespec_tv_nsec_offset,
    offset_of!(libc::timespec, tv_nsec)
);
value!(psx_abi_sockaddr_size, size_of::<libc::sockaddr>());
value!(
    psx_abi_sockaddr_sa_family_offset,
    offset_of!(libc::sockaddr, sa_family)
);
value!(
    psx_abi_sockaddr_storage_size,
    size_of::<libc::sockaddr_storage>()
);
value!(psx_abi_termios_size, size_of::<libc::termios>());
value!(psx_abi_termios_c_cc_offset, offset_of!(libc::termios, c_cc));
value!(psx_abi_evfilt_read, libc::EVFILT_READ);
value!(psx_abi_ev_add, libc::EV_ADD);
value!(psx_abi_af_inet, libc::AF_INET);
value!(psx_abi_af_inet6, libc::AF_INET6);
value!(psx_abi_clock_monotonic, libc::CLOCK_MONOTONIC);

// Keep representative import names visible to an object-file inspector.
// These are addresses only: this library never calls them.
#[unsafe(no_mangle)]
pub static psx_import_stat: unsafe extern "C" fn(
    *const libc::c_char,
    *mut libc::stat,
) -> libc::c_int = libc::stat;

#[unsafe(no_mangle)]
pub static psx_import_fstat: unsafe extern "C" fn(libc::c_int, *mut libc::stat) -> libc::c_int =
    libc::fstat;

#[unsafe(no_mangle)]
pub static psx_import_kevent: unsafe extern "C" fn(
    libc::c_int,
    *const libc::kevent,
    libc::c_int,
    *mut libc::kevent,
    libc::c_int,
    *const libc::timespec,
) -> libc::c_int = libc::kevent;
