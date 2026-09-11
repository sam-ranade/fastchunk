use std::ffi::{c_char, CStr};
use std::ptr;
use tiktoken_rs::{cl100k_base, o200k_base, CoreBPE};

#[repr(C)]
pub struct FastchunkTiktokenToken {
    pub id: u32,
    pub start_byte: usize,
    pub end_byte: usize,
}

pub struct Backend {
    bpe: CoreBPE,
}

fn set_error(error: *mut *mut c_char, message: String) {
    if error.is_null() {
        return;
    }
    let bytes = message.into_bytes();
    let mut bytes = bytes.into_iter().filter(|byte| *byte != 0).collect::<Vec<_>>();
    bytes.push(0);
    unsafe {
        *error = libc::malloc(bytes.len()) as *mut c_char;
        if !(*error).is_null() {
            ptr::copy_nonoverlapping(bytes.as_ptr() as *const c_char, *error, bytes.len());
        }
    }
}

#[no_mangle]
pub extern "C" fn fastchunk_tiktoken_create(
    encoding: *const c_char,
    handle: *mut *mut Backend,
    error: *mut *mut c_char,
) -> i32 {
    if encoding.is_null() || handle.is_null() {
        set_error(error, "encoding and handle are required".to_string());
        return 1;
    }
    let encoding = match unsafe { CStr::from_ptr(encoding) }.to_str() {
        Ok(value) => value,
        Err(_) => {
            set_error(error, "encoding must be valid UTF-8".to_string());
            return 1;
        }
    };
    let bpe = match encoding {
        "cl100k_base" => cl100k_base(),
        "o200k_base" => o200k_base(),
        _ => {
            set_error(error, format!("unsupported encoding: {encoding}"));
            return 3;
        }
    };
    let bpe = match bpe {
        Ok(value) => value,
        Err(message) => {
            set_error(error, format!("failed to load {encoding}: {message}"));
            return 2;
        }
    };
    let backend = Box::new(Backend { bpe });
    unsafe { *handle = Box::into_raw(backend); }
    0
}

#[no_mangle]
pub extern "C" fn fastchunk_tiktoken_encode(
    handle: *mut Backend,
    input: *const u8,
    input_len: usize,
    output: *mut *mut FastchunkTiktokenToken,
    output_len: *mut usize,
    error: *mut *mut c_char,
) -> i32 {
    if handle.is_null() || (input.is_null() && input_len != 0) || output.is_null() || output_len.is_null() {
        set_error(error, "invalid encode arguments".to_string());
        return 1;
    }
    let bytes = unsafe { std::slice::from_raw_parts(input, input_len) };
    let text = match std::str::from_utf8(bytes) {
        Ok(value) => value,
        Err(value) => {
            set_error(error, format!("input is not valid UTF-8 at byte {}", value.valid_up_to()));
            return 4;
        }
    };
    let backend = unsafe { &*handle };
    let ids = backend.bpe.encode_ordinary(text);
    let mut tokens = Vec::with_capacity(ids.len());
    let mut cursor = 0usize;
    for id in ids {
        let decoded = match backend.bpe.decode_bytes(&[id]) {
            Ok(value) => value,
            Err(message) => {
                set_error(error, format!("failed to decode token offset: {message}"));
                return 5;
            }
        };
        if !bytes[cursor..].starts_with(&decoded) {
            set_error(error, format!("could not reconstruct offset for token at byte {cursor}"));
            return 5;
        }
        let end = cursor + decoded.len();
        tokens.push(FastchunkTiktokenToken { id, start_byte: cursor, end_byte: end });
        cursor = end;
    }
    let mut tokens = tokens.into_boxed_slice();
    unsafe {
        *output_len = tokens.len();
        *output = tokens.as_mut_ptr();
    }
    std::mem::forget(tokens);
    0
}

#[no_mangle]
pub extern "C" fn fastchunk_tiktoken_free_tokens(tokens: *mut FastchunkTiktokenToken, len: usize) {
    if !tokens.is_null() {
        unsafe { drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(tokens, len))); }
    }
}

#[no_mangle]
pub extern "C" fn fastchunk_tiktoken_destroy(handle: *mut Backend) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)); }
    }
}

#[no_mangle]
pub extern "C" fn fastchunk_tiktoken_free_error(error: *mut c_char) {
    if !error.is_null() {
        unsafe { libc::free(error as *mut libc::c_void); }
    }
}
