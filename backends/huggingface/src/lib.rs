use std::ffi::{c_char, CStr};
use std::ptr;
use tokenizers::Tokenizer;

#[repr(C)]
pub struct FastchunkHfToken {
    pub id: u32,
    pub start_byte: usize,
    pub end_byte: usize,
}

pub struct Backend {
    tokenizer: Tokenizer,
}

fn set_error(error: *mut *mut c_char, message: String) {
    if error.is_null() {
        return;
    }
    let mut bytes = message.into_bytes().into_iter().filter(|byte| *byte != 0).collect::<Vec<_>>();
    bytes.push(0);
    unsafe {
        *error = libc::malloc(bytes.len()) as *mut c_char;
        if !(*error).is_null() {
            ptr::copy_nonoverlapping(bytes.as_ptr() as *const c_char, *error, bytes.len());
        }
    }
}

#[no_mangle]
pub extern "C" fn fastchunk_huggingface_create(
    path: *const c_char,
    handle: *mut *mut Backend,
    error: *mut *mut c_char,
) -> i32 {
    if path.is_null() || handle.is_null() {
        set_error(error, "tokenizer path and handle are required".to_string());
        return 1;
    }
    let path = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(value) => value,
        Err(_) => {
            set_error(error, "tokenizer path must be valid UTF-8".to_string());
            return 1;
        }
    };
    let tokenizer = match Tokenizer::from_file(path) {
        Ok(value) => value,
        Err(message) => {
            set_error(error, format!("failed to load tokenizer file: {message}"));
            return 2;
        }
    };
    unsafe { *handle = Box::into_raw(Box::new(Backend { tokenizer })); }
    0
}

#[no_mangle]
pub extern "C" fn fastchunk_huggingface_encode(
    handle: *mut Backend,
    input: *const u8,
    input_len: usize,
    output: *mut *mut FastchunkHfToken,
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
    let encoding = match backend.tokenizer.encode(text, true) {
        Ok(value) => value,
        Err(message) => {
            set_error(error, format!("tokenizer encoding failed: {message}"));
            return 5;
        }
    };
    let offsets = encoding.get_offsets();
    let ids = encoding.get_ids();
    let mut tokens = Vec::with_capacity(ids.len());
    for (id, &(start, end)) in ids.iter().zip(offsets.iter()) {
        tokens.push(FastchunkHfToken { id: *id, start_byte: start, end_byte: end });
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
pub extern "C" fn fastchunk_huggingface_free_tokens(tokens: *mut FastchunkHfToken, len: usize) {
    if !tokens.is_null() {
        unsafe { drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(tokens, len))); }
    }
}

#[no_mangle]
pub extern "C" fn fastchunk_huggingface_destroy(handle: *mut Backend) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)); }
    }
}

#[no_mangle]
pub extern "C" fn fastchunk_huggingface_free_error(error: *mut c_char) {
    if !error.is_null() {
        unsafe { libc::free(error as *mut libc::c_void); }
    }
}
