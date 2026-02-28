//! iroh-libevent: C API for iroh with libevent2 integration
//! Supports: iroh core, iroh-gossip, iroh-blobs, iroh-docs

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::PathBuf;
use std::ptr;
use std::sync::Arc;

use bytes::Bytes;
use once_cell::sync::OnceCell;
use parking_lot::Mutex;
use tokio::runtime::Runtime;

use iroh::endpoint::{Connection, Endpoint};
use iroh::protocol::Router;
use iroh::{PublicKey, SecretKey};
use iroh_base::{EndpointAddr, TransportAddr, RelayUrl};

// Protocol imports
use iroh_gossip::Gossip;
use iroh_gossip::TopicId;

// Blobs imports
use iroh_blobs::store::mem::MemStore as BlobMemStore;
use iroh_blobs::store::fs::FsStore as BlobFsStore;
use iroh_blobs::BlobsProtocol;
use iroh_blobs::Hash as BlobHash;
use iroh_blobs::ticket::BlobTicket;

// Docs imports
use iroh_docs::protocol::Docs;
use iroh_docs::{AuthorId, NamespaceId, DocTicket};
use iroh_docs::api::protocol::{ShareMode, AddrInfoOptions};

static RUNTIME: OnceCell<Runtime> = OnceCell::new();

fn get_runtime() -> Option<&'static Runtime> { RUNTIME.get() }

// ============================================================================
// Error Handling
// ============================================================================

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IrohError {
    Ok = 0, InvalidArgument = 1, OutOfMemory = 2, Timeout = 3,
    ConnectionFailed = 4, StreamError = 5, EndpointClosed = 6,
    Pending = 7, Internal = 8, ParseError = 9,
    NotInitialized = 10, AlreadyInitialized = 11,
    BlobNotFound = 12, TopicError = 13, DocError = 14,
}

thread_local! {
    static LAST_ERROR: std::cell::RefCell<Option<String>> = const { std::cell::RefCell::new(None) };
}

fn set_last_error(msg: impl Into<String>) {
    LAST_ERROR.with(|e| { *e.borrow_mut() = Some(msg.into()); });
}

#[no_mangle]
pub extern "C" fn iroh_last_error() -> *mut c_char {
    LAST_ERROR.with(|e| {
        match e.borrow().as_ref() {
            Some(msg) => CString::new(msg.as_str()).map(|s| s.into_raw()).unwrap_or(ptr::null_mut()),
            None => ptr::null_mut(),
        }
    })
}

#[no_mangle]
pub extern "C" fn iroh_string_free(s: *mut c_char) {
    if !s.is_null() { unsafe { drop(CString::from_raw(s)); } }
}

// ============================================================================
// Async Handle
// ============================================================================

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IrohAsyncState { Pending = 0, Ready = 1, Error = 2, Cancelled = 3 }

pub struct IrohAsyncHandle {
    state: Mutex<IrohAsyncState>,
    result: Mutex<Option<Box<dyn std::any::Any + Send>>>,
    error: Mutex<Option<String>>,
    read_fd: c_int,
    write_fd: c_int,
}

unsafe impl Send for IrohAsyncHandle {}
unsafe impl Sync for IrohAsyncHandle {}

impl IrohAsyncHandle {
    fn new() -> Result<Arc<Self>, IrohError> {
        let mut fds: [c_int; 2] = [0; 2];
        #[cfg(unix)]
        {
            if unsafe { libc::pipe(fds.as_mut_ptr()) } != 0 { return Err(IrohError::Internal); }
            unsafe {
                let flags = libc::fcntl(fds[0], libc::F_GETFL);
                libc::fcntl(fds[0], libc::F_SETFL, flags | libc::O_NONBLOCK);
            }
        }
        // On Windows, pipe fds are not used. MinGW CRT fds are incompatible with
        // MSYS libevent. Instead, the C module polls iroh_async_poll() on a timer.
        // read_fd/write_fd remain -1 to signal this to the C layer.
        #[cfg(windows)]
        { fds = [-1, -1]; }
        Ok(Arc::new(Self {
            state: Mutex::new(IrohAsyncState::Pending),
            result: Mutex::new(None), error: Mutex::new(None),
            read_fd: fds[0], write_fd: fds[1],
        }))
    }
    fn complete_with_result<T: Send + 'static>(&self, result: T) {
        *self.result.lock() = Some(Box::new(result));
        *self.state.lock() = IrohAsyncState::Ready;
        self.notify();
    }
    fn complete_with_error(&self, error: impl Into<String>) {
        *self.error.lock() = Some(error.into());
        *self.state.lock() = IrohAsyncState::Error;
        self.notify();
    }
    fn notify(&self) {
        #[cfg(unix)]
        unsafe { let buf: [u8; 1] = [1]; libc::write(self.write_fd, buf.as_ptr() as *const c_void, 1); }
        // On Windows, no pipe notification needed — C module polls via timer.
    }
    fn take_result<T: 'static>(&self) -> Option<T> {
        self.result.lock().take().and_then(|b| b.downcast::<T>().ok()).map(|b| *b)
    }
}

impl Drop for IrohAsyncHandle {
    fn drop(&mut self) {
        #[cfg(unix)]
        unsafe { libc::close(self.read_fd); libc::close(self.write_fd); }
        // On Windows, no pipe fds to close.
    }
}

#[no_mangle]
pub extern "C" fn iroh_async_get_fd(handle: *const IrohAsyncHandle) -> c_int {
    if handle.is_null() { return -1; }
    unsafe { (*handle).read_fd }
}

#[no_mangle]
pub extern "C" fn iroh_async_poll(handle: *const IrohAsyncHandle) -> IrohAsyncState {
    if handle.is_null() { return IrohAsyncState::Error; }
    unsafe {
        #[cfg(unix)]
        { let mut buf: [u8; 64] = [0; 64]; while libc::read((*handle).read_fd, buf.as_mut_ptr() as *mut c_void, 64) > 0 {} }
        *(*handle).state.lock()
    }
}

#[no_mangle]
pub extern "C" fn iroh_async_get_error(handle: *const IrohAsyncHandle) -> *mut c_char {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe {
        match (*handle).error.lock().as_ref() {
            Some(msg) => CString::new(msg.as_str()).map(|s| s.into_raw()).unwrap_or(ptr::null_mut()),
            None => ptr::null_mut(),
        }
    }
}

#[no_mangle]
pub extern "C" fn iroh_async_cancel(handle: *mut IrohAsyncHandle) {
    if handle.is_null() { return; }
    unsafe { *(*handle).state.lock() = IrohAsyncState::Cancelled; (*handle).notify(); }
}

#[no_mangle]
pub extern "C" fn iroh_async_free(handle: *mut IrohAsyncHandle) {
    if !handle.is_null() { unsafe { drop(Arc::from_raw(handle)); } }
}

// ============================================================================
// Initialization
// ============================================================================

#[no_mangle]
pub extern "C" fn iroh_init() -> IrohError {
    if RUNTIME.get().is_some() { return IrohError::AlreadyInitialized; }
    let _ = tracing_subscriber::fmt()
        .with_env_filter(tracing_subscriber::EnvFilter::try_from_default_env()
            .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("error")))
        .try_init();
    match Runtime::new() {
        Ok(rt) => { if RUNTIME.set(rt).is_err() { return IrohError::AlreadyInitialized; } IrohError::Ok }
        Err(e) => { set_last_error(format!("Failed to create runtime: {}", e)); IrohError::Internal }
    }
}

#[no_mangle]
pub extern "C" fn iroh_init_with_threads(num_threads: c_int) -> IrohError {
    if RUNTIME.get().is_some() { return IrohError::AlreadyInitialized; }
    if num_threads <= 0 { return IrohError::InvalidArgument; }
    let _ = tracing_subscriber::fmt()
        .with_env_filter(tracing_subscriber::EnvFilter::try_from_default_env()
            .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("error")))
        .try_init();
    match tokio::runtime::Builder::new_multi_thread().worker_threads(num_threads as usize).enable_all().build() {
        Ok(rt) => { if RUNTIME.set(rt).is_err() { return IrohError::AlreadyInitialized; } IrohError::Ok }
        Err(e) => { set_last_error(format!("Failed to create runtime: {}", e)); IrohError::Internal }
    }
}

// ============================================================================
// Key Types
// ============================================================================

pub struct IrohSecretKey(SecretKey);
pub struct IrohPublicKey(PublicKey);

#[no_mangle]
pub extern "C" fn iroh_secret_key_generate() -> *mut IrohSecretKey {
    Box::into_raw(Box::new(IrohSecretKey(SecretKey::generate(&mut rand::rng()))))
}

#[no_mangle]
pub extern "C" fn iroh_secret_key_from_string(s: *const c_char) -> *mut IrohSecretKey {
    if s.is_null() { set_last_error("NULL string"); return ptr::null_mut(); }
    let s = unsafe { CStr::from_ptr(s) };
    let s = match s.to_str() { Ok(s) => s, Err(e) => { set_last_error(format!("Invalid UTF-8: {}", e)); return ptr::null_mut(); } };
    match s.parse::<SecretKey>() {
        Ok(key) => Box::into_raw(Box::new(IrohSecretKey(key))),
        Err(e) => { set_last_error(format!("Failed to parse secret key: {}", e)); ptr::null_mut() }
    }
}

#[no_mangle]
pub extern "C" fn iroh_secret_key_to_bytes(key: *const IrohSecretKey, out_len: *mut usize) -> *mut u8 {
    if key.is_null() || out_len.is_null() { return ptr::null_mut(); }
    let key = unsafe { &(*key).0 };
    let bytes = key.to_bytes();
    unsafe { *out_len = bytes.len(); }
    let mut boxed = bytes.to_vec().into_boxed_slice();
    let ptr = boxed.as_mut_ptr();
    std::mem::forget(boxed);
    ptr
}

#[no_mangle]
pub extern "C" fn iroh_secret_key_public(key: *const IrohSecretKey) -> *mut IrohPublicKey {
    if key.is_null() { return ptr::null_mut(); }
    Box::into_raw(Box::new(IrohPublicKey(unsafe { &(*key).0 }.public())))
}

#[no_mangle]
pub extern "C" fn iroh_secret_key_free(key: *mut IrohSecretKey) {
    if !key.is_null() { unsafe { drop(Box::from_raw(key)); } }
}

#[no_mangle]
pub extern "C" fn iroh_public_key_from_string(s: *const c_char) -> *mut IrohPublicKey {
    if s.is_null() { set_last_error("NULL string"); return ptr::null_mut(); }
    let s = unsafe { CStr::from_ptr(s) };
    let s = match s.to_str() { Ok(s) => s, Err(e) => { set_last_error(format!("Invalid UTF-8: {}", e)); return ptr::null_mut(); } };
    match s.parse::<PublicKey>() {
        Ok(key) => Box::into_raw(Box::new(IrohPublicKey(key))),
        Err(e) => { set_last_error(format!("Failed to parse public key: {}", e)); ptr::null_mut() }
    }
}

#[no_mangle]
pub extern "C" fn iroh_public_key_to_string(key: *const IrohPublicKey) -> *mut c_char {
    if key.is_null() { return ptr::null_mut(); }
    CString::new(unsafe { &(*key).0 }.to_string()).map(|s| s.into_raw()).unwrap_or(ptr::null_mut())
}

#[no_mangle]
pub extern "C" fn iroh_public_key_free(key: *mut IrohPublicKey) {
    if !key.is_null() { unsafe { drop(Box::from_raw(key)); } }
}

// ============================================================================
// Endpoint Address
// ============================================================================

pub struct IrohEndpointAddr { pub endpoint_id: PublicKey, pub relay_url: Option<String>, pub direct_addrs: Vec<String> }

impl IrohEndpointAddr {
    fn to_endpoint_addr(&self) -> EndpointAddr {
        use std::collections::BTreeSet;
        let mut addrs = BTreeSet::new();
        if let Some(ref relay) = self.relay_url {
            if let Ok(url) = relay.parse::<RelayUrl>() {
                addrs.insert(TransportAddr::Relay(url));
            }
        }
        for da in &self.direct_addrs {
            if let Ok(sa) = da.parse::<std::net::SocketAddr>() {
                addrs.insert(TransportAddr::Ip(sa));
            }
        }
        EndpointAddr { id: self.endpoint_id, addrs }
    }
}

#[no_mangle]
pub extern "C" fn iroh_endpoint_addr_from_public_key(key: *const IrohPublicKey) -> *mut IrohEndpointAddr {
    if key.is_null() { return ptr::null_mut(); }
    Box::into_raw(Box::new(IrohEndpointAddr { endpoint_id: unsafe { (*key).0 }, relay_url: None, direct_addrs: Vec::new() }))
}

#[no_mangle]
pub extern "C" fn iroh_endpoint_addr_with_relay(key: *const IrohPublicKey, relay_url: *const c_char) -> *mut IrohEndpointAddr {
    if key.is_null() { return ptr::null_mut(); }
    let relay = if relay_url.is_null() { None } else { unsafe { CStr::from_ptr(relay_url) }.to_str().ok().map(|s| s.to_string()) };
    Box::into_raw(Box::new(IrohEndpointAddr { endpoint_id: unsafe { (*key).0 }, relay_url: relay, direct_addrs: Vec::new() }))
}

#[no_mangle]
pub extern "C" fn iroh_endpoint_addr_public_key(addr: *const IrohEndpointAddr) -> *mut IrohPublicKey {
    if addr.is_null() { return ptr::null_mut(); }
    Box::into_raw(Box::new(IrohPublicKey(unsafe { &*addr }.endpoint_id)))
}

#[no_mangle]
pub extern "C" fn iroh_endpoint_addr_to_string(addr: *const IrohEndpointAddr) -> *mut c_char {
    if addr.is_null() { return ptr::null_mut(); }
    let addr = unsafe { &*addr };
    let mut s = addr.endpoint_id.to_string();
    if let Some(ref relay) = addr.relay_url { s.push_str("?relay="); s.push_str(relay); }
    CString::new(s).map(|s| s.into_raw()).unwrap_or(ptr::null_mut())
}

#[no_mangle]
pub extern "C" fn iroh_endpoint_addr_free(addr: *mut IrohEndpointAddr) {
    if !addr.is_null() { unsafe { drop(Box::from_raw(addr)); } }
}

#[no_mangle]
pub extern "C" fn iroh_endpoint_addr_from_string(s: *const c_char) -> *mut IrohEndpointAddr {
    if s.is_null() { set_last_error("NULL string"); return ptr::null_mut(); }
    let s = unsafe { CStr::from_ptr(s) };
    let s = match s.to_str() { Ok(s) => s, Err(e) => { set_last_error(format!("Invalid UTF-8: {}", e)); return ptr::null_mut(); } };
    let (key_str, relay_url) = if let Some(idx) = s.find('?') {
        let key_part = &s[..idx];
        let query_part = &s[idx+1..];
        let relay = if query_part.starts_with("relay=") { Some(query_part[6..].to_string()) } else { None };
        (key_part, relay)
    } else { (s, None) };
    match key_str.parse::<PublicKey>() {
        Ok(key) => Box::into_raw(Box::new(IrohEndpointAddr { endpoint_id: key, relay_url, direct_addrs: Vec::new() })),
        Err(e) => { set_last_error(format!("Failed to parse public key: {}", e)); ptr::null_mut() }
    }
}

// ============================================================================
// Endpoint
// ============================================================================

pub struct IrohEndpoint(Endpoint);

// Relay mode enum
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IrohRelayMode {
    /// Disable relay servers completely.
    Disabled = 0,
    /// Use the default relay map, with production relay servers from n0.
    Default = 1,
    /// Use a custom relay URL (set via iroh_endpoint_config_relay_url).
    Custom = 2,
}

/// Opaque endpoint configuration builder
pub struct IrohEndpointConfig {
    // Identity
    secret_key: Option<SecretKey>,
    // ALPNs
    alpns: Vec<Vec<u8>>,
    // Discovery
    discovery_n0: bool,
    discovery_dns: bool,
    discovery_pkarr_publish: bool,
    // Relay
    relay_mode: IrohRelayMode,
    relay_url: Option<String>,
    // Network
    bind_port: Option<u16>,
    bind_addr_v4: Option<std::net::Ipv4Addr>,
    bind_addr_v6: Option<std::net::Ipv6Addr>,
    // Transport
    max_idle_timeout_ms: Option<u64>,
    keep_alive_interval_ms: Option<u64>,
}

impl Default for IrohEndpointConfig {
    fn default() -> Self {
        Self {
            secret_key: None,
            alpns: Vec::new(),
            discovery_n0: true,
            discovery_dns: true,
            discovery_pkarr_publish: true,
            relay_mode: IrohRelayMode::Default,
            relay_url: None,
            bind_port: None,
            bind_addr_v4: None,
            bind_addr_v6: None,
            max_idle_timeout_ms: None,
            keep_alive_interval_ms: None,
        }
    }
}

/// Create a default endpoint configuration.
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_default() -> *mut IrohEndpointConfig {
    Box::into_raw(Box::new(IrohEndpointConfig::default()))
}

/// Free an endpoint configuration.
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_free(config: *mut IrohEndpointConfig) {
    if !config.is_null() { unsafe { drop(Box::from_raw(config)); } }
}

// ============================================================================
// Endpoint Config - Identity
// ============================================================================

/// Set the secret key for the endpoint. Pass NULL to generate a new key.
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_secret_key(config: *mut IrohEndpointConfig, key: *const IrohSecretKey) {
    if config.is_null() { return; }
    let config = unsafe { &mut *config };
    config.secret_key = if key.is_null() { None } else { Some(unsafe { (*key).0.clone() }) };
}

/// Set the secret key from raw bytes (32 bytes).
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_secret_key_bytes(config: *mut IrohEndpointConfig, key_bytes: *const u8, len: usize) {
    if config.is_null() || key_bytes.is_null() || len != 32 { return; }
    let bytes = unsafe { std::slice::from_raw_parts(key_bytes, len) };
    if let Ok(key_array) = <[u8; 32]>::try_from(bytes) {
        let config = unsafe { &mut *config };
        config.secret_key = Some(SecretKey::from_bytes(&key_array));
    }
}

// ============================================================================
// Endpoint Config - ALPNs
// ============================================================================

/// Add an ALPN protocol string.
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_add_alpn(config: *mut IrohEndpointConfig, alpn: *const c_char) {
    if config.is_null() || alpn.is_null() { return; }
    let config = unsafe { &mut *config };
    let alpn_bytes = unsafe { CStr::from_ptr(alpn) }.to_bytes().to_vec();
    config.alpns.push(alpn_bytes);
}

/// Set multiple ALPN protocols at once (replaces any existing ALPNs).
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_set_alpns(config: *mut IrohEndpointConfig, alpns: *const *const c_char, count: usize) {
    if config.is_null() { return; }
    let config = unsafe { &mut *config };
    config.alpns.clear();
    if alpns.is_null() || count == 0 { return; }
    for i in 0..count {
        let ptr = unsafe { *alpns.add(i) };
        if !ptr.is_null() {
            config.alpns.push(unsafe { CStr::from_ptr(ptr) }.to_bytes().to_vec());
        }
    }
}

/// Clear all ALPN protocols.
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_clear_alpns(config: *mut IrohEndpointConfig) {
    if config.is_null() { return; }
    unsafe { &mut *config }.alpns.clear();
}

// ============================================================================
// Endpoint Config - Discovery
// ============================================================================

/// Enable or disable n0 (number zero) discovery service.
/// This uses Iroh's public discovery infrastructure.
/// Default: enabled.
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_discovery_n0(config: *mut IrohEndpointConfig, enabled: bool) {
    if config.is_null() { return; }
    unsafe { &mut *config }.discovery_n0 = enabled;
}

/// Enable or disable DNS discovery.
/// Default: enabled.
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_discovery_dns(config: *mut IrohEndpointConfig, enabled: bool) {
    if config.is_null() { return; }
    unsafe { &mut *config }.discovery_dns = enabled;
}

/// Enable or disable publishing to pkarr (public key addressable resource records).
/// Default: enabled.
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_discovery_pkarr_publish(config: *mut IrohEndpointConfig, enabled: bool) {
    if config.is_null() { return; }
    unsafe { &mut *config }.discovery_pkarr_publish = enabled;
}

// ============================================================================
// Endpoint Config - Relay
// ============================================================================

/// Set the relay mode.
/// - IROH_RELAY_MODE_DISABLED: No relay servers
/// - IROH_RELAY_MODE_DEFAULT: Use default n0 relay servers
/// - IROH_RELAY_MODE_CUSTOM: Use custom relay URL (set via iroh_endpoint_config_relay_url)
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_relay_mode(config: *mut IrohEndpointConfig, mode: IrohRelayMode) {
    if config.is_null() { return; }
    unsafe { &mut *config }.relay_mode = mode;
}

/// Set a custom relay URL. Also sets relay mode to Custom.
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_relay_url(config: *mut IrohEndpointConfig, url: *const c_char) {
    if config.is_null() { return; }
    let config = unsafe { &mut *config };
    if url.is_null() {
        config.relay_url = None;
    } else {
        if let Ok(s) = unsafe { CStr::from_ptr(url) }.to_str() {
            config.relay_url = Some(s.to_string());
            config.relay_mode = IrohRelayMode::Custom;
        }
    }
}

// ============================================================================
// Endpoint Config - Network
// ============================================================================

/// Set the UDP port to bind to. Pass 0 for automatic port selection.
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_bind_port(config: *mut IrohEndpointConfig, port: u16) {
    if config.is_null() { return; }
    unsafe { &mut *config }.bind_port = if port == 0 { None } else { Some(port) };
}

/// Set the IPv4 address to bind to (as a string like "192.168.1.100").
/// Pass NULL to bind to all interfaces.
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_bind_addr_v4(config: *mut IrohEndpointConfig, addr: *const c_char) {
    if config.is_null() { return; }
    let config = unsafe { &mut *config };
    if addr.is_null() {
        config.bind_addr_v4 = None;
    } else {
        if let Ok(s) = unsafe { CStr::from_ptr(addr) }.to_str() {
            if let Ok(ip) = s.parse::<std::net::Ipv4Addr>() {
                config.bind_addr_v4 = Some(ip);
            }
        }
    }
}

/// Set the IPv6 address to bind to (as a string like "::1").
/// Pass NULL to bind to all interfaces.
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_bind_addr_v6(config: *mut IrohEndpointConfig, addr: *const c_char) {
    if config.is_null() { return; }
    let config = unsafe { &mut *config };
    if addr.is_null() {
        config.bind_addr_v6 = None;
    } else {
        if let Ok(s) = unsafe { CStr::from_ptr(addr) }.to_str() {
            if let Ok(ip) = s.parse::<std::net::Ipv6Addr>() {
                config.bind_addr_v6 = Some(ip);
            }
        }
    }
}

// ============================================================================
// Endpoint Config - Transport
// ============================================================================

/// Set the maximum idle timeout in milliseconds.
/// Connections idle for longer than this will be closed.
/// Pass 0 to use the default.
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_max_idle_timeout(config: *mut IrohEndpointConfig, timeout_ms: u64) {
    if config.is_null() { return; }
    unsafe { &mut *config }.max_idle_timeout_ms = if timeout_ms == 0 { None } else { Some(timeout_ms) };
}

/// Set the keep-alive interval in milliseconds.
/// Keep-alive packets are sent to prevent the connection from going idle.
/// Pass 0 to disable keep-alives.
#[no_mangle]
pub extern "C" fn iroh_endpoint_config_keep_alive_interval(config: *mut IrohEndpointConfig, interval_ms: u64) {
    if config.is_null() { return; }
    unsafe { &mut *config }.keep_alive_interval_ms = if interval_ms == 0 { None } else { Some(interval_ms) };
}

// ============================================================================
// Endpoint Creation
// ============================================================================

/// Create an endpoint asynchronously using the provided configuration.
/// The configuration is consumed and should not be used after this call.
#[no_mangle]
pub extern "C" fn iroh_endpoint_create(config: *mut IrohEndpointConfig) -> *mut IrohAsyncHandle {
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    // Take ownership of the config
    let config = if config.is_null() {
        IrohEndpointConfig::default()
    } else {
        unsafe { *Box::from_raw(config) }
    };
    
    runtime.spawn(async move {
        let mut builder = Endpoint::builder();
        
        // Identity
        if let Some(key) = config.secret_key {
            builder = builder.secret_key(key);
        }
        
        // ALPNs
        if !config.alpns.is_empty() {
            builder = builder.alpns(config.alpns);
        }
        
        // Relay mode
        match config.relay_mode {
            IrohRelayMode::Disabled => {
                builder = builder.relay_mode(iroh::endpoint::RelayMode::Disabled);
            }
            IrohRelayMode::Default => {
                // Default is already set
            }
            IrohRelayMode::Custom => {
                if let Some(url_str) = config.relay_url {
                    // Create a RelayMap from a single URL
                    if let Ok(relay_map) = iroh::RelayMap::try_from_iter(std::iter::once(url_str.as_str())) {
                        builder = builder.relay_mode(iroh::endpoint::RelayMode::Custom(relay_map));
                    }
                }
            }
        }
        
        // Network binding - determine the socket address to bind to
        let bind_addr: Option<std::net::SocketAddr> = match (config.bind_port, config.bind_addr_v4, config.bind_addr_v6) {
            (Some(port), Some(v4), _) => Some(std::net::SocketAddr::new(std::net::IpAddr::V4(v4), port)),
            (Some(port), None, Some(v6)) => Some(std::net::SocketAddr::new(std::net::IpAddr::V6(v6), port)),
            (Some(port), None, None) => Some(std::net::SocketAddr::new(std::net::IpAddr::V4(std::net::Ipv4Addr::UNSPECIFIED), port)),
            (None, Some(v4), _) => Some(std::net::SocketAddr::new(std::net::IpAddr::V4(v4), 0)),
            (None, None, Some(v6)) => Some(std::net::SocketAddr::new(std::net::IpAddr::V6(v6), 0)),
            (None, None, None) => None,
        };
        
        if let Some(addr) = bind_addr {
            match builder.bind_addr(addr) {
                Ok(b) => builder = b,
                Err(e) => {
                    handle_clone.complete_with_error(format!("Invalid bind address: {}", e));
                    return;
                }
            }
        }
        
        // Note: Transport config options (max_idle_timeout, keep_alive_interval) 
        // require direct access to quinn::TransportConfig which isn't exposed
        // in the same way in iroh 0.96. These options are currently not applied.
        // TODO: Check if iroh exposes a way to configure these in future versions.
        
        match builder.bind().await {
            Ok(endpoint) => { handle_clone.complete_with_result(IrohEndpoint(endpoint)); }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to bind endpoint: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

#[no_mangle]
pub extern "C" fn iroh_endpoint_create_result(handle: *mut IrohAsyncHandle) -> *mut IrohEndpoint {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohEndpoint>().map(|ep| Box::into_raw(Box::new(ep))).unwrap_or(ptr::null_mut()) }
}

#[no_mangle]
pub extern "C" fn iroh_endpoint_id(endpoint: *const IrohEndpoint) -> *mut IrohPublicKey {
    if endpoint.is_null() { return ptr::null_mut(); }
    Box::into_raw(Box::new(IrohPublicKey(unsafe { &(*endpoint).0 }.id())))
}

#[no_mangle]
pub extern "C" fn iroh_endpoint_secret_key(endpoint: *const IrohEndpoint, out_len: *mut usize) -> *mut u8 {
    if endpoint.is_null() { return ptr::null_mut(); }
    let bytes = unsafe { &(*endpoint).0 }.secret_key().to_bytes();
    if !out_len.is_null() { unsafe { *out_len = 32; } }
    let mut v = bytes.to_vec();
    let ptr = v.as_mut_ptr();
    std::mem::forget(v);
    ptr
}

#[no_mangle]
pub extern "C" fn iroh_endpoint_addr(endpoint: *const IrohEndpoint) -> *mut IrohEndpointAddr {
    if endpoint.is_null() { return ptr::null_mut(); }
    let addr = unsafe { &(*endpoint).0 }.addr();
    let mut relay_url: Option<String> = None;
    let mut direct_addrs: Vec<String> = Vec::new();
    for a in &addr.addrs {
        match a {
            iroh::TransportAddr::Relay(url) => { relay_url = Some(url.to_string()); }
            _ => { direct_addrs.push(format!("{:?}", a)); }
        }
    }
    Box::into_raw(Box::new(IrohEndpointAddr { endpoint_id: addr.id, relay_url, direct_addrs }))
}

#[no_mangle]
pub extern "C" fn iroh_endpoint_set_alpns(endpoint: *const IrohEndpoint, alpns: *const *const c_char, count: usize) {
    if endpoint.is_null() || alpns.is_null() { return; }
    let alpns_vec: Vec<Vec<u8>> = (0..count).filter_map(|i| {
        let ptr = unsafe { *alpns.add(i) };
        if ptr.is_null() { None } else { Some(unsafe { CStr::from_ptr(ptr) }.to_bytes().to_vec()) }
    }).collect();
    unsafe { &(*endpoint).0 }.set_alpns(alpns_vec);
}

#[no_mangle]
pub extern "C" fn iroh_endpoint_close(endpoint: *mut IrohEndpoint) -> *mut IrohAsyncHandle {
    if endpoint.is_null() { return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    let ep = unsafe { Box::from_raw(endpoint) };
    runtime.spawn(async move { ep.0.close().await; handle_clone.complete_with_result(()); });
    handle_ptr as *mut IrohAsyncHandle
}

#[no_mangle]
pub extern "C" fn iroh_endpoint_free(endpoint: *mut IrohEndpoint) {
    if !endpoint.is_null() { unsafe { drop(Box::from_raw(endpoint)); } }
}

// ============================================================================
// Connection
// ============================================================================

pub struct IrohConnection(Arc<Connection>);

#[no_mangle]
pub extern "C" fn iroh_endpoint_connect(endpoint: *const IrohEndpoint, addr: *const IrohEndpointAddr, alpn: *const c_char) -> *mut IrohAsyncHandle {
    if endpoint.is_null() || addr.is_null() || alpn.is_null() { set_last_error("NULL argument"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    let ep_ptr = endpoint as usize;
    let endpoint_addr = unsafe { &*addr }.to_endpoint_addr();
    let alpn = unsafe { CStr::from_ptr(alpn) }.to_bytes().to_vec();
    runtime.spawn(async move {
        let ep = unsafe { &(*(ep_ptr as *const IrohEndpoint)).0 };
        match ep.connect(endpoint_addr, &alpn).await {
            Ok(conn) => { handle_clone.complete_with_result(IrohConnection(Arc::new(conn))); }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to connect: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

#[no_mangle]
pub extern "C" fn iroh_connection_connect_result(handle: *mut IrohAsyncHandle) -> *mut IrohConnection {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohConnection>().map(|c| Box::into_raw(Box::new(c))).unwrap_or(ptr::null_mut()) }
}

#[no_mangle]
pub extern "C" fn iroh_endpoint_accept(endpoint: *const IrohEndpoint) -> *mut IrohAsyncHandle {
    if endpoint.is_null() { set_last_error("NULL endpoint"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    let ep_ptr = endpoint as usize;
    runtime.spawn(async move {
        let ep = unsafe { &(*(ep_ptr as *const IrohEndpoint)).0 };
        match ep.accept().await {
            Some(incoming) => match incoming.await {
                Ok(conn) => { handle_clone.complete_with_result(IrohConnection(Arc::new(conn))); }
                Err(e) => { handle_clone.complete_with_error(format!("Failed to accept: {}", e)); }
            },
            None => { handle_clone.complete_with_error("Endpoint closed"); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

#[no_mangle]
pub extern "C" fn iroh_connection_accept_result(handle: *mut IrohAsyncHandle) -> *mut IrohConnection {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohConnection>().map(|c| Box::into_raw(Box::new(c))).unwrap_or(ptr::null_mut()) }
}

#[no_mangle]
pub extern "C" fn iroh_connection_remote_id(conn: *const IrohConnection) -> *mut IrohPublicKey {
    if conn.is_null() { return ptr::null_mut(); }
    Box::into_raw(Box::new(IrohPublicKey(unsafe { &(*conn).0 }.remote_id())))
}

#[no_mangle]
pub extern "C" fn iroh_connection_alpn(conn: *const IrohConnection) -> *mut c_char {
    if conn.is_null() { return ptr::null_mut(); }
    CString::new(unsafe { &(*conn).0 }.alpn()).map(|s| s.into_raw()).unwrap_or(ptr::null_mut())
}

#[no_mangle]
pub extern "C" fn iroh_connection_close(conn: *mut IrohConnection, code: u32, reason: *const c_char) {
    if conn.is_null() { return; }
    let conn = unsafe { Box::from_raw(conn) };
    let reason_bytes = if reason.is_null() { &[] as &[u8] } else { unsafe { CStr::from_ptr(reason) }.to_bytes() };
    conn.0.close(code.into(), reason_bytes);
}

#[no_mangle]
pub extern "C" fn iroh_connection_free(conn: *mut IrohConnection) {
    if !conn.is_null() { unsafe { drop(Box::from_raw(conn)); } }
}

// ============================================================================
// Streams
// ============================================================================

pub struct IrohSendStream(iroh::endpoint::SendStream);
pub struct IrohRecvStream(iroh::endpoint::RecvStream);
struct IrohBiStream { send: IrohSendStream, recv: IrohRecvStream }

#[no_mangle]
pub extern "C" fn iroh_connection_open_bi(conn: *const IrohConnection) -> *mut IrohAsyncHandle {
    if conn.is_null() { set_last_error("NULL connection"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    let conn = unsafe { (*conn).0.clone() };
    runtime.spawn(async move {
        match conn.open_bi().await {
            Ok((send, recv)) => { handle_clone.complete_with_result(IrohBiStream { send: IrohSendStream(send), recv: IrohRecvStream(recv) }); }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to open stream: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

#[no_mangle]
pub extern "C" fn iroh_stream_open_bi_send_result(handle: *mut IrohAsyncHandle) -> *mut IrohSendStream {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe {
        if *(*handle).state.lock() != IrohAsyncState::Ready { return ptr::null_mut(); }
        
        // Take the result out first to avoid deadlock
        let result_opt = (*handle).result.lock().take();
        
        match result_opt {
            Some(result) => {
                if let Ok(bi) = result.downcast::<IrohBiStream>() {
                    // Store recv back for later retrieval (lock is not held now)
                    *(*handle).result.lock() = Some(Box::new(bi.recv));
                    Box::into_raw(Box::new(bi.send))
                } else { ptr::null_mut() }
            }
            None => ptr::null_mut(),
        }
    }
}

#[no_mangle]
pub extern "C" fn iroh_stream_open_bi_recv_result(handle: *mut IrohAsyncHandle) -> *mut IrohRecvStream {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohRecvStream>().map(|r| Box::into_raw(Box::new(r))).unwrap_or(ptr::null_mut()) }
}

#[no_mangle]
pub extern "C" fn iroh_connection_accept_bi(conn: *const IrohConnection) -> *mut IrohAsyncHandle {
    if conn.is_null() { set_last_error("NULL connection"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    let conn = unsafe { (*conn).0.clone() };
    runtime.spawn(async move {
        match conn.accept_bi().await {
            Ok((send, recv)) => { handle_clone.complete_with_result(IrohBiStream { send: IrohSendStream(send), recv: IrohRecvStream(recv) }); }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to accept stream: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

#[no_mangle]
pub extern "C" fn iroh_connection_open_uni(conn: *const IrohConnection) -> *mut IrohAsyncHandle {
    if conn.is_null() { set_last_error("NULL connection"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    let conn = unsafe { (*conn).0.clone() };
    runtime.spawn(async move {
        match conn.open_uni().await {
            Ok(send) => { handle_clone.complete_with_result(IrohSendStream(send)); }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to open stream: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

#[no_mangle]
pub extern "C" fn iroh_stream_open_uni_result(handle: *mut IrohAsyncHandle) -> *mut IrohSendStream {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohSendStream>().map(|s| Box::into_raw(Box::new(s))).unwrap_or(ptr::null_mut()) }
}

#[no_mangle]
pub extern "C" fn iroh_connection_accept_uni(conn: *const IrohConnection) -> *mut IrohAsyncHandle {
    if conn.is_null() { set_last_error("NULL connection"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    let conn = unsafe { (*conn).0.clone() };
    runtime.spawn(async move {
        match conn.accept_uni().await {
            Ok(recv) => { handle_clone.complete_with_result(IrohRecvStream(recv)); }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to accept stream: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

#[no_mangle]
pub extern "C" fn iroh_stream_accept_uni_result(handle: *mut IrohAsyncHandle) -> *mut IrohRecvStream {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohRecvStream>().map(|r| Box::into_raw(Box::new(r))).unwrap_or(ptr::null_mut()) }
}

// ============================================================================
// Stream I/O
// ============================================================================

#[repr(C)]
pub struct IrohReadResult { pub data: *mut u8, pub len: usize, pub finished: bool }

#[no_mangle]
pub extern "C" fn iroh_send_stream_write(stream: *mut IrohSendStream, data: *const u8, len: usize) -> *mut IrohAsyncHandle {
    if stream.is_null() || (data.is_null() && len > 0) { set_last_error("NULL argument"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    let data_vec = if len > 0 { unsafe { std::slice::from_raw_parts(data, len).to_vec() } } else { Vec::new() };
    let stream_ptr = stream as usize;
    runtime.spawn(async move {
        let stream = unsafe { &mut (*(stream_ptr as *mut IrohSendStream)).0 };
        match stream.write_all(&data_vec).await {
            Ok(()) => { handle_clone.complete_with_result(data_vec.len()); }
            Err(e) => { handle_clone.complete_with_error(format!("Write failed: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

#[no_mangle]
pub extern "C" fn iroh_send_stream_write_result(handle: *mut IrohAsyncHandle) -> isize {
    if handle.is_null() { return -1; }
    unsafe { (*handle).take_result::<usize>().map(|n| n as isize).unwrap_or(-1) }
}

#[no_mangle]
pub extern "C" fn iroh_send_stream_finish(stream: *mut IrohSendStream) -> IrohError {
    if stream.is_null() { return IrohError::InvalidArgument; }
    match unsafe { &mut (*stream).0 }.finish() {
        Ok(()) => IrohError::Ok,
        Err(e) => { set_last_error(format!("Finish failed: {}", e)); IrohError::StreamError }
    }
}

#[no_mangle]
pub extern "C" fn iroh_send_stream_free(stream: *mut IrohSendStream) {
    if !stream.is_null() { unsafe { drop(Box::from_raw(stream)); } }
}

#[no_mangle]
pub extern "C" fn iroh_recv_stream_read(stream: *mut IrohRecvStream, max_len: usize) -> *mut IrohAsyncHandle {
    if stream.is_null() { set_last_error("NULL stream"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    let stream_ptr = stream as usize;
    runtime.spawn(async move {
        let stream = unsafe { &mut (*(stream_ptr as *mut IrohRecvStream)).0 };
        let mut buf = vec![0u8; max_len];
        match stream.read(&mut buf).await {
            Ok(Some(n)) => { buf.truncate(n); handle_clone.complete_with_result((buf, false)); }
            Ok(None) => { handle_clone.complete_with_result((Vec::<u8>::new(), true)); }
            Err(e) => { handle_clone.complete_with_error(format!("Read failed: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

#[no_mangle]
pub extern "C" fn iroh_recv_stream_read_to_end(stream: *mut IrohRecvStream, max_len: usize) -> *mut IrohAsyncHandle {
    if stream.is_null() { set_last_error("NULL stream"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    let stream_ptr = stream as usize;
    runtime.spawn(async move {
        let stream = unsafe { &mut (*(stream_ptr as *mut IrohRecvStream)).0 };
        match stream.read_to_end(max_len).await {
            Ok(data) => { handle_clone.complete_with_result((data.to_vec(), true)); }
            Err(e) => { handle_clone.complete_with_error(format!("Read failed: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

#[no_mangle]
pub extern "C" fn iroh_recv_stream_read_result(handle: *mut IrohAsyncHandle) -> IrohReadResult {
    if handle.is_null() { return IrohReadResult { data: ptr::null_mut(), len: 0, finished: false }; }
    unsafe {
        match (*handle).take_result::<(Vec<u8>, bool)>() {
            Some((data, finished)) => {
                let len = data.len();
                let ptr = if len > 0 { let mut b = data.into_boxed_slice(); let p = b.as_mut_ptr(); std::mem::forget(b); p } else { ptr::null_mut() };
                IrohReadResult { data: ptr, len, finished }
            }
            None => IrohReadResult { data: ptr::null_mut(), len: 0, finished: false },
        }
    }
}

#[no_mangle]
pub extern "C" fn iroh_bytes_free(data: *mut u8, len: usize) {
    if !data.is_null() && len > 0 { unsafe { let _ = Vec::from_raw_parts(data, len, len); } }
}

#[no_mangle]
pub extern "C" fn iroh_recv_stream_free(stream: *mut IrohRecvStream) {
    if !stream.is_null() { unsafe { drop(Box::from_raw(stream)); } }
}

// ============================================================================
// Datagrams
// ============================================================================

#[no_mangle]
pub extern "C" fn iroh_connection_send_datagram(conn: *const IrohConnection, data: *const u8, len: usize) -> IrohError {
    if conn.is_null() || (data.is_null() && len > 0) { return IrohError::InvalidArgument; }
    let conn = unsafe { &(*conn).0 };
    let data_bytes = if len > 0 { Bytes::copy_from_slice(unsafe { std::slice::from_raw_parts(data, len) }) } else { Bytes::new() };
    match conn.send_datagram(data_bytes) {
        Ok(()) => IrohError::Ok,
        Err(e) => { set_last_error(format!("Failed to send datagram: {}", e)); IrohError::ConnectionFailed }
    }
}

#[no_mangle]
pub extern "C" fn iroh_connection_recv_datagram(conn: *const IrohConnection) -> *mut IrohAsyncHandle {
    if conn.is_null() { set_last_error("NULL connection"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    let conn = unsafe { (*conn).0.clone() };
    runtime.spawn(async move {
        match conn.read_datagram().await {
            Ok(data) => { handle_clone.complete_with_result(data.to_vec()); }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to receive datagram: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

#[no_mangle]
pub extern "C" fn iroh_connection_recv_datagram_result(handle: *mut IrohAsyncHandle, out_len: *mut usize) -> *mut u8 {
    if handle.is_null() || out_len.is_null() { return ptr::null_mut(); }
    unsafe {
        match (*handle).take_result::<Vec<u8>>() {
            Some(data) => {
                let len = data.len(); *out_len = len;
                if len > 0 { let mut b = data.into_boxed_slice(); let p = b.as_mut_ptr(); std::mem::forget(b); p } else { ptr::null_mut() }
            }
            None => { *out_len = 0; ptr::null_mut() }
        }
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

#[no_mangle]
pub extern "C" fn iroh_endpoint_wait_online(endpoint: *const IrohEndpoint) -> *mut IrohAsyncHandle {
    if endpoint.is_null() { set_last_error("NULL endpoint"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    let ep_ptr = endpoint as usize;
    runtime.spawn(async move {
        let ep = unsafe { &(*(ep_ptr as *const IrohEndpoint)).0 };
        ep.online().await;
        handle_clone.complete_with_result(());
    });
    handle_ptr as *mut IrohAsyncHandle
}

#[no_mangle]
pub extern "C" fn iroh_endpoint_wait_online_result(handle: *mut IrohAsyncHandle) -> IrohError {
    if handle.is_null() { return IrohError::InvalidArgument; }
    unsafe { (*handle).take_result::<()>().map(|_| IrohError::Ok).unwrap_or(IrohError::Pending) }
}

// ============================================================================
// IROH-GOSSIP PROTOCOL
// ============================================================================

/// Opaque handle for a gossip topic ID (32 bytes)
pub struct IrohTopicId(TopicId);

/// Opaque handle for the gossip protocol
pub struct IrohGossip(Gossip);

/// Opaque handle for a gossip topic subscription
pub struct IrohGossipTopic(iroh_gossip::api::GossipTopic);

/// Create a topic ID from 32 bytes
#[no_mangle]
pub extern "C" fn iroh_topic_id_from_bytes(bytes: *const u8) -> *mut IrohTopicId {
    if bytes.is_null() { set_last_error("NULL bytes"); return ptr::null_mut(); }
    let bytes_arr: [u8; 32] = unsafe { std::slice::from_raw_parts(bytes, 32) }.try_into().unwrap_or([0u8; 32]);
    Box::into_raw(Box::new(IrohTopicId(TopicId::from_bytes(bytes_arr))))
}

/// Create a topic ID from a name string by hashing it with BLAKE3
/// This is useful for creating human-readable topic names
#[no_mangle]
pub extern "C" fn iroh_topic_id_from_name(name: *const c_char) -> *mut IrohTopicId {
    if name.is_null() { set_last_error("NULL name"); return ptr::null_mut(); }
    let name_cstr = unsafe { CStr::from_ptr(name) };
    let name_bytes = name_cstr.to_bytes();
    // Hash the name to get 32 bytes for the topic ID
    let hash = blake3::hash(name_bytes);
    let bytes_arr: [u8; 32] = *hash.as_bytes();
    Box::into_raw(Box::new(IrohTopicId(TopicId::from_bytes(bytes_arr))))
}

/// Create a topic ID from a string (parsed as hex or base32)
#[no_mangle]
pub extern "C" fn iroh_topic_id_from_string(s: *const c_char) -> *mut IrohTopicId {
    if s.is_null() { set_last_error("NULL string"); return ptr::null_mut(); }
    let s = unsafe { CStr::from_ptr(s) };
    let s = match s.to_str() { Ok(s) => s, Err(e) => { set_last_error(format!("Invalid UTF-8: {}", e)); return ptr::null_mut(); } };
    match s.parse::<TopicId>() {
        Ok(topic) => Box::into_raw(Box::new(IrohTopicId(topic))),
        Err(e) => { set_last_error(format!("Failed to parse topic: {}", e)); ptr::null_mut() }
    }
}

/// Convert a topic ID to string
#[no_mangle]
pub extern "C" fn iroh_topic_id_to_string(topic: *const IrohTopicId) -> *mut c_char {
    if topic.is_null() { return ptr::null_mut(); }
    CString::new(unsafe { &(*topic).0 }.to_string()).map(|s| s.into_raw()).unwrap_or(ptr::null_mut())
}

/// Get the raw bytes of a topic ID (32 bytes)
#[no_mangle]
pub extern "C" fn iroh_topic_id_to_bytes(topic: *const IrohTopicId, out_len: *mut usize) -> *mut u8 {
    if topic.is_null() || out_len.is_null() { return ptr::null_mut(); }
    let bytes = unsafe { &(*topic).0 }.as_bytes();
    unsafe { *out_len = 32; }
    let mut boxed = bytes.to_vec().into_boxed_slice();
    let ptr = boxed.as_mut_ptr();
    std::mem::forget(boxed);
    ptr
}

/// Free a topic ID
#[no_mangle]
pub extern "C" fn iroh_topic_id_free(topic: *mut IrohTopicId) {
    if !topic.is_null() { unsafe { drop(Box::from_raw(topic)); } }
}

/// Create a gossip protocol instance (async because it needs tokio runtime)
#[no_mangle]
pub extern "C" fn iroh_gossip_create(endpoint: *const IrohEndpoint) -> *mut IrohAsyncHandle {
    if endpoint.is_null() { set_last_error("NULL endpoint"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let ep = unsafe { (*endpoint).0.clone() };
    
    runtime.spawn(async move {
        let gossip = Gossip::builder().spawn(ep);
        handle_clone.complete_with_result(IrohGossip(gossip));
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the gossip instance from a completed create operation
#[no_mangle]
pub extern "C" fn iroh_gossip_create_result(handle: *mut IrohAsyncHandle) -> *mut IrohGossip {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohGossip>().map(|g| Box::into_raw(Box::new(g))).unwrap_or(ptr::null_mut()) }
}

/// Get the ALPN for gossip protocol (for router setup)
#[no_mangle]
pub extern "C" fn iroh_gossip_alpn() -> *mut c_char {
    CString::new(iroh_gossip::ALPN).map(|s| s.into_raw()).unwrap_or(ptr::null_mut())
}

/// Subscribe to a gossip topic
#[no_mangle]
pub extern "C" fn iroh_gossip_subscribe(
    gossip: *const IrohGossip,
    topic: *const IrohTopicId,
    bootstrap_peers: *const *const IrohPublicKey,
    bootstrap_count: usize,
) -> *mut IrohAsyncHandle {
    if gossip.is_null() || topic.is_null() { set_last_error("NULL argument"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let gossip_ptr = gossip as usize;
    let topic_id = unsafe { (*topic).0 };
    
    let peers: Vec<PublicKey> = if bootstrap_peers.is_null() || bootstrap_count == 0 {
        Vec::new()
    } else {
        (0..bootstrap_count).filter_map(|i| {
            let ptr = unsafe { *bootstrap_peers.add(i) };
            if ptr.is_null() { None } else { Some(unsafe { (*ptr).0 }) }
        }).collect()
    };
    
    runtime.spawn(async move {
        let gossip = unsafe { &(*(gossip_ptr as *const IrohGossip)).0 };
        match gossip.subscribe(topic_id, peers).await {
            Ok(topic) => {
                handle_clone.complete_with_result(IrohGossipTopic(topic));
            }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to subscribe: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the gossip topic from a completed subscribe operation
#[no_mangle]
pub extern "C" fn iroh_gossip_subscribe_result(handle: *mut IrohAsyncHandle) -> *mut IrohGossipTopic {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohGossipTopic>().map(|t| Box::into_raw(Box::new(t))).unwrap_or(ptr::null_mut()) }
}

/// Broadcast a message to all peers in the topic
#[no_mangle]
pub extern "C" fn iroh_gossip_broadcast(topic: *const IrohGossipTopic, data: *const u8, len: usize) -> *mut IrohAsyncHandle {
    if topic.is_null() || (data.is_null() && len > 0) { set_last_error("NULL argument"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let data_bytes = if len > 0 { 
        Bytes::copy_from_slice(unsafe { std::slice::from_raw_parts(data, len) })
    } else { 
        Bytes::new() 
    };
    
    let topic_ptr = topic as usize;
    runtime.spawn(async move {
        let topic = unsafe { &mut (*(topic_ptr as *mut IrohGossipTopic)).0 };
        match topic.broadcast(data_bytes).await {
            Ok(()) => { handle_clone.complete_with_result(()); }
            Err(e) => { handle_clone.complete_with_error(format!("Broadcast failed: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Check if broadcast completed successfully
#[no_mangle]
pub extern "C" fn iroh_gossip_broadcast_result(handle: *mut IrohAsyncHandle) -> IrohError {
    if handle.is_null() { return IrohError::InvalidArgument; }
    unsafe { (*handle).take_result::<()>().map(|_| IrohError::Ok).unwrap_or(IrohError::Pending) }
}

/// Internal struct for gossip event result (Send-safe)
struct GossipEventResult {
    event_type: u32,
    data: Vec<u8>,
    peer: Option<PublicKey>,
}

/// Gossip event types
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IrohGossipEventType {
    Received = 0,
    NeighborUp = 1,
    NeighborDown = 2,
    Joined = 3,
    Error = 4,
}

/// Result of receiving a gossip event
#[repr(C)]
pub struct IrohGossipEvent {
    pub event_type: IrohGossipEventType,
    pub data: *mut u8,
    pub data_len: usize,
    pub peer: *mut IrohPublicKey,
}

/// Receive the next gossip event (async)
#[no_mangle]
pub extern "C" fn iroh_gossip_recv(topic: *mut IrohGossipTopic) -> *mut IrohAsyncHandle {
    if topic.is_null() { set_last_error("NULL topic"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let topic_ptr = topic as usize;
    runtime.spawn(async move {
        use n0_future::StreamExt;
        let topic = unsafe { &mut (*(topic_ptr as *mut IrohGossipTopic)).0 };
        match topic.next().await {
            Some(Ok(event)) => {
                use iroh_gossip::api::Event;
                let result = match event {
                    Event::Received(msg) => {
                        GossipEventResult {
                            event_type: 0,
                            data: msg.content.to_vec(),
                            peer: Some(msg.delivered_from),
                        }
                    }
                    Event::NeighborUp(peer) => {
                        GossipEventResult {
                            event_type: 1,
                            data: Vec::new(),
                            peer: Some(peer),
                        }
                    }
                    Event::NeighborDown(peer) => {
                        GossipEventResult {
                            event_type: 2,
                            data: Vec::new(),
                            peer: Some(peer),
                        }
                    }
                    Event::Lagged => {
                        GossipEventResult {
                            event_type: 4,
                            data: Vec::new(),
                            peer: None,
                        }
                    }
                };
                handle_clone.complete_with_result(result);
            }
            Some(Err(e)) => { handle_clone.complete_with_error(format!("Recv failed: {}", e)); }
            None => { handle_clone.complete_with_error("Stream closed"); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the gossip event from a completed recv operation
#[no_mangle]
pub extern "C" fn iroh_gossip_recv_result(handle: *mut IrohAsyncHandle) -> IrohGossipEvent {
    let empty = IrohGossipEvent { event_type: IrohGossipEventType::Error, data: ptr::null_mut(), data_len: 0, peer: ptr::null_mut() };
    if handle.is_null() { return empty; }
    unsafe {
        match (*handle).take_result::<GossipEventResult>() {
            Some(result) => {
                let event_type = match result.event_type {
                    0 => IrohGossipEventType::Received,
                    1 => IrohGossipEventType::NeighborUp,
                    2 => IrohGossipEventType::NeighborDown,
                    3 => IrohGossipEventType::Joined,
                    _ => IrohGossipEventType::Error,
                };
                let (data_ptr, data_len) = if result.data.is_empty() {
                    (ptr::null_mut(), 0)
                } else {
                    let len = result.data.len();
                    let mut b = result.data.into_boxed_slice();
                    let p = b.as_mut_ptr();
                    std::mem::forget(b);
                    (p, len)
                };
                let peer_ptr = result.peer.map(|p| Box::into_raw(Box::new(IrohPublicKey(p)))).unwrap_or(ptr::null_mut());
                IrohGossipEvent { event_type, data: data_ptr, data_len, peer: peer_ptr }
            }
            None => empty,
        }
    }
}

/// Free gossip event data
#[no_mangle]
pub extern "C" fn iroh_gossip_event_free(event: *mut IrohGossipEvent) {
    if event.is_null() { return; }
    unsafe {
        let e = &*event;
        if !e.data.is_null() && e.data_len > 0 {
            let _ = Vec::from_raw_parts(e.data, e.data_len, e.data_len);
        }
        if !e.peer.is_null() {
            drop(Box::from_raw(e.peer));
        }
    }
}

/// Free gossip topic
#[no_mangle]
pub extern "C" fn iroh_gossip_topic_free(topic: *mut IrohGossipTopic) {
    if !topic.is_null() { unsafe { drop(Box::from_raw(topic)); } }
}

/// Free gossip protocol
#[no_mangle]
pub extern "C" fn iroh_gossip_free(gossip: *mut IrohGossip) {
    if !gossip.is_null() { unsafe { drop(Box::from_raw(gossip)); } }
}

// ============================================================================
// ROUTER (for multi-protocol support)
// ============================================================================

/// Opaque handle for the protocol router
pub struct IrohRouter(Router);

/// Combined gossip and router result
struct GossipWithRouter {
    gossip: Gossip,
    router: Router,
}

/// Create gossip protocol with router (required for gossip to work properly)
/// This sets up the router to accept incoming gossip connections
#[no_mangle]
pub extern "C" fn iroh_gossip_create_with_router(endpoint: *const IrohEndpoint) -> *mut IrohAsyncHandle {
    if endpoint.is_null() { set_last_error("NULL endpoint"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let ep = unsafe { (*endpoint).0.clone() };
    
    runtime.spawn(async move {
        // Create gossip protocol
        let gossip = Gossip::builder().spawn(ep.clone());
        
        // Create router and register gossip protocol
        let router = Router::builder(ep)
            .accept(iroh_gossip::ALPN, gossip.clone())
            .spawn();
        
        handle_clone.complete_with_result(GossipWithRouter { gossip, router });
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the gossip instance from a completed create_with_router operation
#[no_mangle]
pub extern "C" fn iroh_gossip_from_router_result(handle: *mut IrohAsyncHandle) -> *mut IrohGossip {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe {
        if *(*handle).state.lock() != IrohAsyncState::Ready { return ptr::null_mut(); }
        
        // Take the result out of the lock first to avoid deadlock
        let result_opt = (*handle).result.lock().take();
        
        match result_opt {
            Some(result) => {
                if let Ok(gwr) = result.downcast::<GossipWithRouter>() {
                    // Store the router back for later retrieval (lock is not held now)
                    *(*handle).result.lock() = Some(Box::new(IrohRouter(gwr.router)));
                    Box::into_raw(Box::new(IrohGossip(gwr.gossip)))
                } else { ptr::null_mut() }
            }
            None => ptr::null_mut(),
        }
    }
}

/// Get the router from a completed create_with_router operation (call after getting gossip)
#[no_mangle]
pub extern "C" fn iroh_router_from_gossip_result(handle: *mut IrohAsyncHandle) -> *mut IrohRouter {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohRouter>().map(|r| Box::into_raw(Box::new(r))).unwrap_or(ptr::null_mut()) }
}

/// Create a router for the endpoint (without any protocols)
#[no_mangle]
pub extern "C" fn iroh_router_create(endpoint: *const IrohEndpoint) -> *mut IrohAsyncHandle {
    if endpoint.is_null() { set_last_error("NULL endpoint"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let ep = unsafe { (*endpoint).0.clone() };
    
    runtime.spawn(async move {
        let router = Router::builder(ep).spawn();
        handle_clone.complete_with_result(IrohRouter(router));
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get router from completed creation
#[no_mangle]
pub extern "C" fn iroh_router_create_result(handle: *mut IrohAsyncHandle) -> *mut IrohRouter {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohRouter>().map(|r| Box::into_raw(Box::new(r))).unwrap_or(ptr::null_mut()) }
}

/// Shutdown the router gracefully
#[no_mangle]
pub extern "C" fn iroh_router_shutdown(router: *mut IrohRouter) -> *mut IrohAsyncHandle {
    if router.is_null() { set_last_error("NULL router"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let router = unsafe { Box::from_raw(router) };
    runtime.spawn(async move {
        match router.0.shutdown().await {
            Ok(()) => { handle_clone.complete_with_result(()); }
            Err(e) => { handle_clone.complete_with_error(format!("Shutdown failed: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Check if shutdown completed
#[no_mangle]
pub extern "C" fn iroh_router_shutdown_result(handle: *mut IrohAsyncHandle) -> IrohError {
    if handle.is_null() { return IrohError::InvalidArgument; }
    unsafe { (*handle).take_result::<()>().map(|_| IrohError::Ok).unwrap_or(IrohError::Pending) }
}

/// Free router (does not shutdown gracefully)
#[no_mangle]
pub extern "C" fn iroh_router_free(router: *mut IrohRouter) {
    if !router.is_null() { unsafe { drop(Box::from_raw(router)); } }
}

// ============================================================================
// IROH-BLOBS PROTOCOL
// ============================================================================

/// Opaque handle for a blob hash (BLAKE3, 32 bytes)
pub struct IrohBlobHash(BlobHash);

/// Enum to hold either memory or filesystem blob store
#[derive(Clone)]
pub enum BlobStoreInner {
    Memory(BlobMemStore),
    Filesystem(BlobFsStore),
}

impl BlobStoreInner {
    async fn add_slice(&self, data: &[u8]) -> Result<BlobHash, String> {
        match self {
            BlobStoreInner::Memory(s) => s.add_slice(data).await.map(|t| t.hash).map_err(|e| e.to_string()),
            BlobStoreInner::Filesystem(s) => s.add_slice(data).await.map(|t| t.hash).map_err(|e| e.to_string()),
        }
    }
    
    fn reader(&self, hash: BlobHash) -> iroh_blobs::api::blobs::BlobReader {
        match self {
            BlobStoreInner::Memory(s) => s.reader(hash),
            BlobStoreInner::Filesystem(s) => s.reader(hash),
        }
    }
    
    async fn has(&self, hash: BlobHash) -> Result<bool, String> {
        match self {
            BlobStoreInner::Memory(s) => s.has(hash).await.map_err(|e| e.to_string()),
            BlobStoreInner::Filesystem(s) => s.has(hash).await.map_err(|e| e.to_string()),
        }
    }
}

/// Opaque handle for the blob store (memory or filesystem)
pub struct IrohBlobStore(BlobStoreInner);

/// Opaque handle for the blobs protocol
pub struct IrohBlobsProtocol(BlobsProtocol);

/// Opaque handle for a blob ticket
pub struct IrohBlobTicket(BlobTicket);

/// Combined blobs protocol and router result
struct BlobsWithRouter {
    store: BlobStoreInner,
    blobs: BlobsProtocol,
    router: Router,
}

/// Create a blob hash from a string (hex or base32)
#[no_mangle]
pub extern "C" fn iroh_blob_hash_from_string(s: *const c_char) -> *mut IrohBlobHash {
    if s.is_null() { set_last_error("NULL string"); return ptr::null_mut(); }
    let s = unsafe { CStr::from_ptr(s) };
    let s = match s.to_str() { Ok(s) => s, Err(e) => { set_last_error(format!("Invalid UTF-8: {}", e)); return ptr::null_mut(); } };
    match s.parse::<BlobHash>() {
        Ok(hash) => Box::into_raw(Box::new(IrohBlobHash(hash))),
        Err(e) => { set_last_error(format!("Failed to parse hash: {}", e)); ptr::null_mut() }
    }
}

/// Convert a blob hash to string
#[no_mangle]
pub extern "C" fn iroh_blob_hash_to_string(hash: *const IrohBlobHash) -> *mut c_char {
    if hash.is_null() { return ptr::null_mut(); }
    CString::new(unsafe { &(*hash).0 }.to_string()).map(|s| s.into_raw()).unwrap_or(ptr::null_mut())
}

/// Get the raw bytes of a blob hash (32 bytes)
#[no_mangle]
pub extern "C" fn iroh_blob_hash_to_bytes(hash: *const IrohBlobHash, out_len: *mut usize) -> *mut u8 {
    if hash.is_null() || out_len.is_null() { return ptr::null_mut(); }
    let bytes = unsafe { &(*hash).0 }.as_bytes();
    unsafe { *out_len = 32; }
    let mut boxed = bytes.to_vec().into_boxed_slice();
    let ptr = boxed.as_mut_ptr();
    std::mem::forget(boxed);
    ptr
}

/// Free a blob hash
#[no_mangle]
pub extern "C" fn iroh_blob_hash_free(hash: *mut IrohBlobHash) {
    if !hash.is_null() { unsafe { drop(Box::from_raw(hash)); } }
}

/// Create blobs protocol with router (required for serving blobs)
/// This sets up an in-memory store and router to accept blob requests
#[no_mangle]
pub extern "C" fn iroh_blobs_create_with_router(endpoint: *const IrohEndpoint) -> *mut IrohAsyncHandle {
    if endpoint.is_null() { set_last_error("NULL endpoint"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let ep = unsafe { (*endpoint).0.clone() };
    
    runtime.spawn(async move {
        // Create in-memory blob store
        let store = BlobMemStore::new();
        
        // Create blobs protocol
        let blobs = BlobsProtocol::new(&store, None);
        
        // Create router and register blobs protocol
        let router = Router::builder(ep)
            .accept(iroh_blobs::ALPN, blobs.clone())
            .spawn();
        
        handle_clone.complete_with_result(BlobsWithRouter { 
            store: BlobStoreInner::Memory(store), 
            blobs, 
            router 
        });
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Create blobs protocol with router using persistent filesystem storage
/// The path specifies where blob data will be stored on disk.
/// This sets up a filesystem-backed store and router to accept blob requests.
#[no_mangle]
pub extern "C" fn iroh_blobs_create_with_router_persistent(
    endpoint: *const IrohEndpoint,
    path: *const c_char,
) -> *mut IrohAsyncHandle {
    if endpoint.is_null() { set_last_error("NULL endpoint"); return ptr::null_mut(); }
    if path.is_null() { set_last_error("NULL path"); return ptr::null_mut(); }
    
    let path_str = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(s) => s,
        Err(e) => { set_last_error(format!("Invalid UTF-8 in path: {}", e)); return ptr::null_mut(); }
    };
    let storage_path = PathBuf::from(path_str);
    
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let ep = unsafe { (*endpoint).0.clone() };
    
    runtime.spawn(async move {
        // Create the directory if it doesn't exist
        if let Err(e) = tokio::fs::create_dir_all(&storage_path).await {
            handle_clone.complete_with_error(format!("Failed to create storage directory {:?}: {}", storage_path, e));
            return;
        }
        
        // Create filesystem blob store
        match BlobFsStore::load(&storage_path).await {
            Ok(store) => {
                // Create blobs protocol
                let blobs = BlobsProtocol::new(&store, None);
                
                // Create router and register blobs protocol
                let router = Router::builder(ep)
                    .accept(iroh_blobs::ALPN, blobs.clone())
                    .spawn();
                
                handle_clone.complete_with_result(BlobsWithRouter { 
                    store: BlobStoreInner::Filesystem(store), 
                    blobs, 
                    router 
                });
            }
            Err(e) => {
                handle_clone.complete_with_error(format!("Failed to create blob store at {:?}: {}", storage_path, e));
            }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the blob store from a completed create_with_router operation
#[no_mangle]
pub extern "C" fn iroh_blobs_store_from_router_result(handle: *mut IrohAsyncHandle) -> *mut IrohBlobStore {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe {
        if *(*handle).state.lock() != IrohAsyncState::Ready { return ptr::null_mut(); }
        
        let result_opt = (*handle).result.lock().take();
        
        match result_opt {
            Some(result) => {
                if let Ok(bwr) = result.downcast::<BlobsWithRouter>() {
                    // Store the blobs and router back for later retrieval
                    *(*handle).result.lock() = Some(Box::new((bwr.blobs, IrohRouter(bwr.router))));
                    Box::into_raw(Box::new(IrohBlobStore(bwr.store)))
                } else { ptr::null_mut() }
            }
            None => ptr::null_mut(),
        }
    }
}

/// Get the blobs protocol from a completed create_with_router operation (call after getting store)
#[no_mangle]
pub extern "C" fn iroh_blobs_protocol_from_router_result(handle: *mut IrohAsyncHandle) -> *mut IrohBlobsProtocol {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe {
        let result_opt = (*handle).result.lock().take();
        
        match result_opt {
            Some(result) => {
                if let Ok(boxed) = result.downcast::<(BlobsProtocol, IrohRouter)>() {
                    let (blobs, router) = *boxed;
                    // Store the router back for later retrieval
                    *(*handle).result.lock() = Some(Box::new(router));
                    Box::into_raw(Box::new(IrohBlobsProtocol(blobs)))
                } else { ptr::null_mut() }
            }
            None => ptr::null_mut(),
        }
    }
}

/// Get the router from a completed blobs create_with_router operation (call after getting blobs)
#[no_mangle]
pub extern "C" fn iroh_router_from_blobs_result(handle: *mut IrohAsyncHandle) -> *mut IrohRouter {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohRouter>().map(|r| Box::into_raw(Box::new(r))).unwrap_or(ptr::null_mut()) }
}

/// Add data to the blob store and get its hash
#[no_mangle]
pub extern "C" fn iroh_blobs_add_bytes(
    store: *const IrohBlobStore,
    data: *const u8,
    len: usize,
) -> *mut IrohAsyncHandle {
    if store.is_null() || (data.is_null() && len > 0) { set_last_error("NULL argument"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let data_vec = if len > 0 { unsafe { std::slice::from_raw_parts(data, len).to_vec() } } else { Vec::new() };
    let store_inner = unsafe { (*store).0.clone() };
    
    runtime.spawn(async move {
        match store_inner.add_slice(&data_vec).await {
            Ok(hash) => {
                handle_clone.complete_with_result(IrohBlobHash(hash));
            }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to add blob: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the blob hash from a completed add operation
#[no_mangle]
pub extern "C" fn iroh_blobs_add_result(handle: *mut IrohAsyncHandle) -> *mut IrohBlobHash {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohBlobHash>().map(|h| Box::into_raw(Box::new(h))).unwrap_or(ptr::null_mut()) }
}

/// Add a file to the blob store
#[no_mangle]
pub extern "C" fn iroh_blobs_add_file(
    store: *const IrohBlobStore,
    path: *const c_char,
) -> *mut IrohAsyncHandle {
    if store.is_null() || path.is_null() { set_last_error("NULL argument"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let path_str = unsafe { CStr::from_ptr(path) };
    let path = match path_str.to_str() {
        Ok(s) => PathBuf::from(s),
        Err(e) => { 
            handle_clone.complete_with_error(format!("Invalid path: {}", e));
            return handle_ptr as *mut IrohAsyncHandle;
        }
    };
    
    let store_inner = unsafe { (*store).0.clone() };
    
    runtime.spawn(async move {
        // Read file contents
        match tokio::fs::read(&path).await {
            Ok(data) => {
                match store_inner.add_slice(&data).await {
                    Ok(hash) => {
                        handle_clone.complete_with_result(IrohBlobHash(hash));
                    }
                    Err(e) => { handle_clone.complete_with_error(format!("Failed to add blob: {}", e)); }
                }
            }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to read file: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Create a blob ticket for sharing
#[no_mangle]
pub extern "C" fn iroh_blobs_create_ticket(
    endpoint: *const IrohEndpoint,
    hash: *const IrohBlobHash,
) -> *mut IrohBlobTicket {
    if endpoint.is_null() || hash.is_null() { set_last_error("NULL argument"); return ptr::null_mut(); }
    
    let ep = unsafe { &(*endpoint).0 };
    let hash = unsafe { (*hash).0 };
    let addr = ep.addr();
    
    let ticket = BlobTicket::new(addr, hash, iroh_blobs::BlobFormat::Raw);
    Box::into_raw(Box::new(IrohBlobTicket(ticket)))
}

/// Parse a blob ticket from string
#[no_mangle]
pub extern "C" fn iroh_blob_ticket_from_string(s: *const c_char) -> *mut IrohBlobTicket {
    if s.is_null() { set_last_error("NULL string"); return ptr::null_mut(); }
    let s = unsafe { CStr::from_ptr(s) };
    let s = match s.to_str() { Ok(s) => s, Err(e) => { set_last_error(format!("Invalid UTF-8: {}", e)); return ptr::null_mut(); } };
    match s.parse::<BlobTicket>() {
        Ok(ticket) => Box::into_raw(Box::new(IrohBlobTicket(ticket))),
        Err(e) => { set_last_error(format!("Failed to parse ticket: {}", e)); ptr::null_mut() }
    }
}

/// Convert a blob ticket to string
#[no_mangle]
pub extern "C" fn iroh_blob_ticket_to_string(ticket: *const IrohBlobTicket) -> *mut c_char {
    if ticket.is_null() { return ptr::null_mut(); }
    CString::new(unsafe { &(*ticket).0 }.to_string()).map(|s| s.into_raw()).unwrap_or(ptr::null_mut())
}

/// Get the hash from a blob ticket
#[no_mangle]
pub extern "C" fn iroh_blob_ticket_hash(ticket: *const IrohBlobTicket) -> *mut IrohBlobHash {
    if ticket.is_null() { return ptr::null_mut(); }
    Box::into_raw(Box::new(IrohBlobHash(unsafe { &(*ticket).0 }.hash())))
}

/// Get the endpoint address from a blob ticket
#[no_mangle]
pub extern "C" fn iroh_blob_ticket_addr(ticket: *const IrohBlobTicket) -> *mut IrohEndpointAddr {
    if ticket.is_null() { return ptr::null_mut(); }
    let addr = unsafe { &(*ticket).0 }.addr();
    let mut relay_url: Option<String> = None;
    let mut direct_addrs: Vec<String> = Vec::new();
    for a in &addr.addrs {
        match a {
            iroh::TransportAddr::Relay(url) => { relay_url = Some(url.to_string()); }
            _ => { direct_addrs.push(format!("{:?}", a)); }
        }
    }
    Box::into_raw(Box::new(IrohEndpointAddr { endpoint_id: addr.id, relay_url, direct_addrs }))
}

/// Free a blob ticket
#[no_mangle]
pub extern "C" fn iroh_blob_ticket_free(ticket: *mut IrohBlobTicket) {
    if !ticket.is_null() { unsafe { drop(Box::from_raw(ticket)); } }
}

/// Read blob data by hash
#[no_mangle]
pub extern "C" fn iroh_blobs_read(
    store: *const IrohBlobStore,
    hash: *const IrohBlobHash,
) -> *mut IrohAsyncHandle {
    if store.is_null() || hash.is_null() { set_last_error("NULL argument"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let store_inner = unsafe { (*store).0.clone() };
    let hash = unsafe { (*hash).0 };
    
    runtime.spawn(async move {
        use tokio::io::AsyncReadExt;
        let mut reader = store_inner.reader(hash);
        let mut data = Vec::new();
        match reader.read_to_end(&mut data).await {
            Ok(_) => {
                handle_clone.complete_with_result(data);
            }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to read blob: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the blob data from a completed read operation
#[no_mangle]
pub extern "C" fn iroh_blobs_read_result(handle: *mut IrohAsyncHandle, out_len: *mut usize) -> *mut u8 {
    if handle.is_null() || out_len.is_null() { return ptr::null_mut(); }
    unsafe {
        match (*handle).take_result::<Vec<u8>>() {
            Some(data) => {
                let len = data.len();
                *out_len = len;
                if len > 0 {
                    let mut b = data.into_boxed_slice();
                    let p = b.as_mut_ptr();
                    std::mem::forget(b);
                    p
                } else { ptr::null_mut() }
            }
            None => { *out_len = 0; ptr::null_mut() }
        }
    }
}

/// Download a blob from a remote peer using a ticket
#[no_mangle]
pub extern "C" fn iroh_blobs_download(
    blobs: *const IrohBlobsProtocol,
    endpoint: *const IrohEndpoint,
    ticket: *const IrohBlobTicket,
) -> *mut IrohAsyncHandle {
    if blobs.is_null() || endpoint.is_null() || ticket.is_null() { 
        set_last_error("NULL argument"); 
        return ptr::null_mut(); 
    }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let blobs = unsafe { (*blobs).0.clone() };
    let ep = unsafe { (*endpoint).0.clone() };
    let ticket = unsafe { (*ticket).0.clone() };
    
    runtime.spawn(async move {
        let hash = ticket.hash();
        let addr = ticket.addr().clone();

        // Connect directly to the provider using the full EndpointAddr (which includes
        // relay URLs), rather than the downloader API which only uses the node ID.
        let conn = match ep.connect(addr, iroh_blobs::ALPN).await {
            Ok(c) => c,
            Err(e) => {
                handle_clone.complete_with_error(format!("Failed to connect to provider: {}", e));
                return;
            }
        };

        // Use execute_get to fetch the blob data into our local store
        let request = iroh_blobs::protocol::GetRequest::blob(hash);
        match blobs.remote().execute_get(conn, request).await {
            Ok(_stats) => {
                handle_clone.complete_with_result(IrohBlobHash(hash));
            }
            Err(e) => {
                handle_clone.complete_with_error(format!("Failed to download blob: {}", e));
            }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the blob hash from a completed download operation
#[no_mangle]
pub extern "C" fn iroh_blobs_download_result(handle: *mut IrohAsyncHandle) -> *mut IrohBlobHash {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohBlobHash>().map(|h| Box::into_raw(Box::new(h))).unwrap_or(ptr::null_mut()) }
}

/// Free blob store
#[no_mangle]
pub extern "C" fn iroh_blobs_store_free(store: *mut IrohBlobStore) {
    if !store.is_null() { unsafe { drop(Box::from_raw(store)); } }
}

/// Free blobs protocol
#[no_mangle]
pub extern "C" fn iroh_blobs_protocol_free(blobs: *mut IrohBlobsProtocol) {
    if !blobs.is_null() { unsafe { drop(Box::from_raw(blobs)); } }
}

/// Get the ALPN for blobs protocol
#[no_mangle]
pub extern "C" fn iroh_blobs_alpn() -> *mut c_char {
    CString::new(iroh_blobs::ALPN).map(|s| s.into_raw()).unwrap_or(ptr::null_mut())
}

// ============================================================================
// IROH-DOCS PROTOCOL
// ============================================================================

/// Opaque handle for the docs protocol
pub struct IrohDocs {
    docs: Docs,
    blobs_store: BlobStoreInner,
}

/// Opaque handle for an author ID
pub struct IrohAuthorId(AuthorId);

/// Opaque handle for a namespace ID (document ID)
pub struct IrohNamespaceId(NamespaceId);

/// Opaque handle for a document ticket
pub struct IrohDocTicket(DocTicket);

/// Combined docs protocol result with all required components
struct DocsWithRouter {
    docs: Docs,
    blobs_store: BlobStoreInner,
    blobs: BlobsProtocol,
    gossip: Gossip,
    router: Router,
}

/// Create docs protocol with all dependencies (blobs, gossip, router)
/// Docs requires both blobs and gossip protocols to function
/// Uses in-memory storage (data is lost when the process exits)
#[no_mangle]
pub extern "C" fn iroh_docs_create_with_router(endpoint: *const IrohEndpoint) -> *mut IrohAsyncHandle {
    if endpoint.is_null() { set_last_error("NULL endpoint"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let ep = unsafe { (*endpoint).0.clone() };
    
    runtime.spawn(async move {
        // Create in-memory blob store
        let blobs_store = BlobMemStore::new();
        
        // Create blobs protocol
        let blobs = BlobsProtocol::new(&blobs_store, None);
        
        // Create gossip protocol
        let gossip = Gossip::builder().spawn(ep.clone());
        
        // Create docs protocol - requires endpoint, blobs store, and gossip
        match Docs::memory().spawn(ep.clone(), blobs_store.clone().into(), gossip.clone()).await {
            Ok(docs) => {
                // Create router and register all protocols
                let router = Router::builder(ep)
                    .accept(iroh_blobs::ALPN, blobs.clone())
                    .accept(iroh_gossip::ALPN, gossip.clone())
                    .accept(iroh_docs::ALPN, docs.clone())
                    .spawn();
                
                handle_clone.complete_with_result(DocsWithRouter {
                    docs,
                    blobs_store: BlobStoreInner::Memory(blobs_store),
                    blobs,
                    gossip,
                    router,
                });
            }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to create docs: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Create docs protocol with persistent filesystem storage
/// The path specifies the directory where data will be stored on disk.
/// Both document metadata and blob content are persisted.
/// Docs requires both blobs and gossip protocols to function.
#[no_mangle]
pub extern "C" fn iroh_docs_create_with_router_persistent(
    endpoint: *const IrohEndpoint,
    path: *const c_char,
) -> *mut IrohAsyncHandle {
    if endpoint.is_null() { set_last_error("NULL endpoint"); return ptr::null_mut(); }
    if path.is_null() { set_last_error("NULL path"); return ptr::null_mut(); }
    
    let path_str = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(s) => s,
        Err(e) => { set_last_error(format!("Invalid UTF-8 in path: {}", e)); return ptr::null_mut(); }
    };
    let storage_path = PathBuf::from(path_str);
    
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let ep = unsafe { (*endpoint).0.clone() };
    
    runtime.spawn(async move {
        // Create subdirectories for blobs and docs
        let blobs_path = storage_path.join("blobs");
        let docs_path = storage_path.join("docs");
        let docs_path_for_error = docs_path.clone();
        
        // Create the directories if they don't exist
        if let Err(e) = tokio::fs::create_dir_all(&blobs_path).await {
            handle_clone.complete_with_error(format!("Failed to create blobs directory {:?}: {}", blobs_path, e));
            return;
        }
        if let Err(e) = tokio::fs::create_dir_all(&docs_path).await {
            handle_clone.complete_with_error(format!("Failed to create docs directory {:?}: {}", docs_path, e));
            return;
        }
        
        // Create filesystem blob store
        match BlobFsStore::load(&blobs_path).await {
            Ok(blobs_store) => {
                // Create blobs protocol
                let blobs = BlobsProtocol::new(&blobs_store, None);
                
                // Create gossip protocol
                let gossip = Gossip::builder().spawn(ep.clone());
                
                // Create docs protocol with persistent storage
                match Docs::persistent(docs_path).spawn(ep.clone(), blobs_store.clone().into(), gossip.clone()).await {
                    Ok(docs) => {
                        // Create router and register all protocols
                        let router = Router::builder(ep)
                            .accept(iroh_blobs::ALPN, blobs.clone())
                            .accept(iroh_gossip::ALPN, gossip.clone())
                            .accept(iroh_docs::ALPN, docs.clone())
                            .spawn();
                        
                        handle_clone.complete_with_result(DocsWithRouter {
                            docs,
                            blobs_store: BlobStoreInner::Filesystem(blobs_store),
                            blobs,
                            gossip,
                            router,
                        });
                    }
                    Err(e) => { 
                        handle_clone.complete_with_error(format!("Failed to create docs at {:?}: {}", docs_path_for_error, e)); 
                    }
                }
            }
            Err(e) => {
                handle_clone.complete_with_error(format!("Failed to create blob store at {:?}: {}", blobs_path, e));
            }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the docs protocol from a completed create_with_router operation
#[no_mangle]
pub extern "C" fn iroh_docs_from_router_result(handle: *mut IrohAsyncHandle) -> *mut IrohDocs {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe {
        if *(*handle).state.lock() != IrohAsyncState::Ready { return ptr::null_mut(); }
        
        let result_opt = (*handle).result.lock().take();
        
        match result_opt {
            Some(result) => {
                if let Ok(dwr) = result.downcast::<DocsWithRouter>() {
                    // Store remaining components for later retrieval
                    // We need to clone the store for the IrohDocs struct
                    let store_for_docs = match &dwr.blobs_store {
                        BlobStoreInner::Memory(s) => BlobStoreInner::Memory(s.clone()),
                        BlobStoreInner::Filesystem(s) => BlobStoreInner::Filesystem(s.clone()),
                    };
                    *(*handle).result.lock() = Some(Box::new((dwr.blobs_store, dwr.blobs, dwr.gossip, IrohRouter(dwr.router))));
                    Box::into_raw(Box::new(IrohDocs { docs: dwr.docs, blobs_store: store_for_docs }))
                } else { ptr::null_mut() }
            }
            None => ptr::null_mut(),
        }
    }
}

/// Get the blob store from a completed docs create_with_router operation (call after getting docs)
#[no_mangle]
pub extern "C" fn iroh_blobs_store_from_docs_result(handle: *mut IrohAsyncHandle) -> *mut IrohBlobStore {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe {
        let result_opt = (*handle).result.lock().take();
        
        match result_opt {
            Some(result) => {
                if let Ok(boxed) = result.downcast::<(BlobStoreInner, BlobsProtocol, Gossip, IrohRouter)>() {
                    let (store, blobs, gossip, router) = *boxed;
                    *(*handle).result.lock() = Some(Box::new((blobs, gossip, router)));
                    Box::into_raw(Box::new(IrohBlobStore(store)))
                } else { ptr::null_mut() }
            }
            None => ptr::null_mut(),
        }
    }
}

/// Get the router from a completed docs create_with_router operation (call last)
#[no_mangle]
pub extern "C" fn iroh_router_from_docs_result(handle: *mut IrohAsyncHandle) -> *mut IrohRouter {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe {
        let result_opt = (*handle).result.lock().take();
        
        match result_opt {
            Some(result) => {
                if let Ok(boxed) = result.downcast::<(BlobsProtocol, Gossip, IrohRouter)>() {
                    let (_blobs, _gossip, router) = *boxed;
                    Box::into_raw(Box::new(router))
                } else { ptr::null_mut() }
            }
            None => ptr::null_mut(),
        }
    }
}

/// Create a new author for signing document entries
#[no_mangle]
pub extern "C" fn iroh_docs_create_author(docs: *const IrohDocs) -> *mut IrohAsyncHandle {
    if docs.is_null() { set_last_error("NULL docs"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let docs = unsafe { (*docs).docs.clone() };
    
    runtime.spawn(async move {
        match docs.author_create().await {
            Ok(author_id) => {
                handle_clone.complete_with_result(IrohAuthorId(author_id));
            }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to create author: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the author ID from a completed create_author operation
#[no_mangle]
pub extern "C" fn iroh_docs_create_author_result(handle: *mut IrohAsyncHandle) -> *mut IrohAuthorId {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohAuthorId>().map(|a| Box::into_raw(Box::new(a))).unwrap_or(ptr::null_mut()) }
}

/// Convert an author ID to string
#[no_mangle]
pub extern "C" fn iroh_author_id_to_string(author: *const IrohAuthorId) -> *mut c_char {
    if author.is_null() { return ptr::null_mut(); }
    CString::new(unsafe { &(*author).0 }.to_string()).map(|s| s.into_raw()).unwrap_or(ptr::null_mut())
}

/// Parse an author ID from string
#[no_mangle]
pub extern "C" fn iroh_author_id_from_string(s: *const c_char) -> *mut IrohAuthorId {
    if s.is_null() { set_last_error("NULL string"); return ptr::null_mut(); }
    let s = unsafe { CStr::from_ptr(s) };
    let s = match s.to_str() { Ok(s) => s, Err(e) => { set_last_error(format!("Invalid UTF-8: {}", e)); return ptr::null_mut(); } };
    match s.parse::<AuthorId>() {
        Ok(author) => Box::into_raw(Box::new(IrohAuthorId(author))),
        Err(e) => { set_last_error(format!("Failed to parse author ID: {}", e)); ptr::null_mut() }
    }
}

/// Free an author ID
#[no_mangle]
pub extern "C" fn iroh_author_id_free(author: *mut IrohAuthorId) {
    if !author.is_null() { unsafe { drop(Box::from_raw(author)); } }
}

/// Convert a namespace ID to string
#[no_mangle]
pub extern "C" fn iroh_namespace_id_to_string(namespace: *const IrohNamespaceId) -> *mut c_char {
    if namespace.is_null() { return ptr::null_mut(); }
    CString::new(unsafe { &(*namespace).0 }.to_string()).map(|s| s.into_raw()).unwrap_or(ptr::null_mut())
}

/// Parse a namespace ID from string
#[no_mangle]
pub extern "C" fn iroh_namespace_id_from_string(s: *const c_char) -> *mut IrohNamespaceId {
    if s.is_null() { set_last_error("NULL string"); return ptr::null_mut(); }
    let s = unsafe { CStr::from_ptr(s) };
    let s = match s.to_str() { Ok(s) => s, Err(e) => { set_last_error(format!("Invalid UTF-8: {}", e)); return ptr::null_mut(); } };
    match s.parse::<NamespaceId>() {
        Ok(ns) => Box::into_raw(Box::new(IrohNamespaceId(ns))),
        Err(e) => { set_last_error(format!("Failed to parse namespace ID: {}", e)); ptr::null_mut() }
    }
}

/// Free a namespace ID
#[no_mangle]
pub extern "C" fn iroh_namespace_id_free(namespace: *mut IrohNamespaceId) {
    if !namespace.is_null() { unsafe { drop(Box::from_raw(namespace)); } }
}

/// Create a new document (namespace)
#[no_mangle]
pub extern "C" fn iroh_docs_create_doc(docs: *const IrohDocs) -> *mut IrohAsyncHandle {
    if docs.is_null() { set_last_error("NULL docs"); return ptr::null_mut(); }
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let docs = unsafe { (*docs).docs.clone() };
    
    runtime.spawn(async move {
        match docs.create().await {
            Ok(doc) => {
                let namespace_id = doc.id();
                handle_clone.complete_with_result(IrohNamespaceId(namespace_id));
            }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to create doc: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the namespace ID from a completed create_doc operation
#[no_mangle]
pub extern "C" fn iroh_docs_create_doc_result(handle: *mut IrohAsyncHandle) -> *mut IrohNamespaceId {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohNamespaceId>().map(|n| Box::into_raw(Box::new(n))).unwrap_or(ptr::null_mut()) }
}

/// Set an entry in a document (key-value pair)
#[no_mangle]
pub extern "C" fn iroh_docs_set(
    docs: *const IrohDocs,
    namespace: *const IrohNamespaceId,
    author: *const IrohAuthorId,
    key: *const c_char,
    value: *const u8,
    value_len: usize,
) -> *mut IrohAsyncHandle {
    if docs.is_null() || namespace.is_null() || author.is_null() || key.is_null() { 
        set_last_error("NULL argument"); 
        return ptr::null_mut(); 
    }
    if value.is_null() && value_len > 0 { set_last_error("NULL value with non-zero length"); return ptr::null_mut(); }
    
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let docs = unsafe { (*docs).docs.clone() };
    let namespace = unsafe { (*namespace).0 };
    let author = unsafe { (*author).0 };
    let key_str = unsafe { CStr::from_ptr(key) };
    let key_bytes = key_str.to_bytes().to_vec();
    let value_bytes = if value_len > 0 { unsafe { std::slice::from_raw_parts(value, value_len).to_vec() } } else { Vec::new() };
    
    runtime.spawn(async move {
        match docs.open(namespace).await {
            Ok(Some(doc)) => {
                match doc.set_bytes(author, key_bytes, value_bytes).await {
                    Ok(_hash) => {
                        handle_clone.complete_with_result(());
                    }
                    Err(e) => { handle_clone.complete_with_error(format!("Failed to set entry: {}", e)); }
                }
            }
            Ok(None) => { handle_clone.complete_with_error("Document not found".to_string()); }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to open doc: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Set multiple entries in a document from parallel arrays of keys and values
#[no_mangle]
pub extern "C" fn iroh_docs_set_multi(
    docs: *const IrohDocs,
    namespace: *const IrohNamespaceId,
    author: *const IrohAuthorId,
    keys: *const *const c_char,
    values: *const *const u8,
    value_lens: *const usize,
    count: usize,
) -> *mut IrohAsyncHandle {
    if docs.is_null() || namespace.is_null() || author.is_null() || keys.is_null() || values.is_null() || value_lens.is_null() {
        set_last_error("NULL argument");
        return ptr::null_mut();
    }
    if count == 0 { set_last_error("count must be > 0"); return ptr::null_mut(); }

    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);

    let docs = unsafe { (*docs).docs.clone() };
    let namespace = unsafe { (*namespace).0 };
    let author = unsafe { (*author).0 };

    // Copy all keys and values into owned Vecs before moving into the async block
    let mut entries: Vec<(Vec<u8>, Vec<u8>)> = Vec::with_capacity(count);
    for i in 0..count {
        unsafe {
            let key_ptr = *keys.add(i);
            if key_ptr.is_null() {
                set_last_error("NULL key pointer");
                // Free the handle we allocated
                let _ = Arc::from_raw(handle_ptr);
                return ptr::null_mut();
            }
            let key_bytes = CStr::from_ptr(key_ptr).to_bytes().to_vec();
            let val_ptr = *values.add(i);
            let val_len = *value_lens.add(i);
            let val_bytes = if val_len > 0 {
                if val_ptr.is_null() {
                    set_last_error("NULL value pointer with non-zero length");
                    let _ = Arc::from_raw(handle_ptr);
                    return ptr::null_mut();
                }
                std::slice::from_raw_parts(val_ptr, val_len).to_vec()
            } else {
                Vec::new()
            };
            entries.push((key_bytes, val_bytes));
        }
    }

    runtime.spawn(async move {
        match docs.open(namespace).await {
            Ok(Some(doc)) => {
                for (key_bytes, value_bytes) in entries {
                    match doc.set_bytes(author, key_bytes, value_bytes).await {
                        Ok(_hash) => {}
                        Err(e) => {
                            handle_clone.complete_with_error(format!("Failed to set entry: {}", e));
                            return;
                        }
                    }
                }
                handle_clone.complete_with_result(());
            }
            Ok(None) => { handle_clone.complete_with_error("Document not found".to_string()); }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to open doc: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Check if a set operation completed successfully
#[no_mangle]
pub extern "C" fn iroh_docs_set_result(handle: *mut IrohAsyncHandle) -> IrohError {
    if handle.is_null() { return IrohError::InvalidArgument; }
    unsafe { (*handle).take_result::<()>().map(|_| IrohError::Ok).unwrap_or(IrohError::Pending) }
}

/// Set an entry and wait for peers to have a chance to sync.
/// This writes the entry and then waits the specified time for peers to sync.
/// 
/// The wait allows connected peers to:
/// 1. Receive the gossip announcement about the new entry
/// 2. Sync the entry metadata  
/// 3. Download the blob content
///
/// timeout_secs: Time to wait after writing (recommended: 3-5 seconds for LAN, 10+ for WAN)
/// If timeout_secs is 0, returns immediately after writing (same as iroh_docs_set).
#[no_mangle]
pub extern "C" fn iroh_docs_set_and_sync(
    docs: *const IrohDocs,
    namespace: *const IrohNamespaceId,
    author: *const IrohAuthorId,
    key: *const c_char,
    value: *const u8,
    value_len: usize,
    timeout_secs: u32,
) -> *mut IrohAsyncHandle {
    if docs.is_null() || namespace.is_null() || author.is_null() || key.is_null() { 
        set_last_error("NULL argument"); 
        return ptr::null_mut(); 
    }
    if value.is_null() && value_len > 0 { set_last_error("NULL value with non-zero length"); return ptr::null_mut(); }
    
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let docs = unsafe { (*docs).docs.clone() };
    let namespace = unsafe { (*namespace).0 };
    let author = unsafe { (*author).0 };
    let key_str = unsafe { CStr::from_ptr(key) };
    let key_bytes = key_str.to_bytes().to_vec();
    let value_bytes = if value_len > 0 { unsafe { std::slice::from_raw_parts(value, value_len).to_vec() } } else { Vec::new() };
    let wait_duration = tokio::time::Duration::from_secs(timeout_secs as u64);
    
    runtime.spawn(async move {
        // Open the document and write the entry
        match docs.open(namespace).await {
            Ok(Some(doc)) => {
                match doc.set_bytes(author, key_bytes, value_bytes).await {
                    Ok(hash) => {
                        tracing::info!("Entry written with hash {}", hash);
                        
                        if timeout_secs > 0 {
                            // Wait for peers to sync
                            // During this time, the router continues to handle:
                            // - Incoming sync requests from peers
                            // - Blob download requests from peers
                            tracing::info!("Waiting {} seconds for peers to sync...", timeout_secs);
                            tokio::time::sleep(wait_duration).await;
                            tracing::info!("Wait complete");
                        }
                        
                        handle_clone.complete_with_result(());
                    }
                    Err(e) => { handle_clone.complete_with_error(format!("Failed to set entry: {}", e)); }
                }
            }
            Ok(None) => { handle_clone.complete_with_error("Document not found".to_string()); }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to open doc: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Check if a set_and_sync operation completed successfully
#[no_mangle]
pub extern "C" fn iroh_docs_set_and_sync_result(handle: *mut IrohAsyncHandle) -> IrohError {
    if handle.is_null() { return IrohError::InvalidArgument; }
    unsafe { (*handle).take_result::<()>().map(|_| IrohError::Ok).unwrap_or(IrohError::Pending) }
}

/// Get an entry from a document by key
#[no_mangle]
pub extern "C" fn iroh_docs_get(
    docs: *const IrohDocs,
    namespace: *const IrohNamespaceId,
    author: *const IrohAuthorId,
    key: *const c_char,
) -> *mut IrohAsyncHandle {
    if docs.is_null() || namespace.is_null() || author.is_null() || key.is_null() { 
        set_last_error("NULL argument"); 
        return ptr::null_mut(); 
    }
    
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let docs_handle = unsafe { &(*docs) };
    let docs = docs_handle.docs.clone();
    let blobs_store = docs_handle.blobs_store.clone();
    let namespace = unsafe { (*namespace).0 };
    let author = unsafe { (*author).0 };
    let key_str = unsafe { CStr::from_ptr(key) };
    let key_bytes: Bytes = key_str.to_bytes().to_vec().into();
    
    runtime.spawn(async move {
        match docs.open(namespace).await {
            Ok(Some(doc)) => {
                match doc.get_exact(author, key_bytes, false).await {
                    Ok(Some(entry)) => {
                        // Read the content from the blob store using the entry's content hash
                        use tokio::io::AsyncReadExt;
                        let hash = entry.content_hash();
                        let mut reader = blobs_store.reader(hash);
                        let mut content = Vec::new();
                        match reader.read_to_end(&mut content).await {
                            Ok(_) => {
                                handle_clone.complete_with_result(content);
                            }
                            Err(e) => { handle_clone.complete_with_error(format!("Failed to read content: {}", e)); }
                        }
                    }
                    Ok(None) => {
                        // Entry not found - return empty
                        handle_clone.complete_with_result(Vec::<u8>::new());
                    }
                    Err(e) => { handle_clone.complete_with_error(format!("Failed to get entry: {}", e)); }
                }
            }
            Ok(None) => { handle_clone.complete_with_error("Document not found".to_string()); }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to open doc: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the value from a completed get operation
#[no_mangle]
pub extern "C" fn iroh_docs_get_result(handle: *mut IrohAsyncHandle, out_len: *mut usize) -> *mut u8 {
    if handle.is_null() || out_len.is_null() { return ptr::null_mut(); }
    unsafe {
        match (*handle).take_result::<Vec<u8>>() {
            Some(data) => {
                let len = data.len();
                *out_len = len;
                if len > 0 {
                    let mut b = data.into_boxed_slice();
                    let p = b.as_mut_ptr();
                    std::mem::forget(b);
                    p
                } else { ptr::null_mut() }
            }
            None => { *out_len = 0; ptr::null_mut() }
        }
    }
}

/// Get the latest entry for a key from any author
/// This is the recommended function for key-value store usage
#[no_mangle]
pub extern "C" fn iroh_docs_get_latest(
    docs: *const IrohDocs,
    namespace: *const IrohNamespaceId,
    key: *const c_char,
) -> *mut IrohAsyncHandle {
    if docs.is_null() || namespace.is_null() || key.is_null() { 
        set_last_error("NULL argument"); 
        return ptr::null_mut(); 
    }
    
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let docs_handle = unsafe { &(*docs) };
    let docs = docs_handle.docs.clone();
    let blobs_store = docs_handle.blobs_store.clone();
    let namespace = unsafe { (*namespace).0 };
    let key_str = unsafe { CStr::from_ptr(key) };
    let key_bytes: Bytes = key_str.to_bytes().to_vec().into();
    
    runtime.spawn(async move {
        match docs.open(namespace).await {
            Ok(Some(doc)) => {
                // Use get_one to get the latest entry for this key from any author
                match doc.get_one(iroh_docs::store::Query::single_latest_per_key().key_exact(&key_bytes).build()).await {
                    Ok(Some(entry)) => {
                        // Read the content from the blob store using the entry's content hash
                        use tokio::io::AsyncReadExt;
                        let hash = entry.content_hash();
                        
                        // Check if the content is available locally
                        match blobs_store.has(hash).await {
                            Ok(false) => {
                                // Content not available - entry synced but blob not downloaded yet
                                handle_clone.complete_with_error(format!(
                                    "Content not available locally (hash: {}). The entry was synced but the blob content has not been downloaded. Try waiting longer for sync.", 
                                    hash
                                ));
                                return;
                            }
                            Err(e) => {
                                handle_clone.complete_with_error(format!("Failed to check content availability: {}", e));
                                return;
                            }
                            Ok(true) => {
                                // Content is available, proceed to read
                            }
                        }
                        
                        let mut reader = blobs_store.reader(hash);
                        let mut content = Vec::new();
                        match reader.read_to_end(&mut content).await {
                            Ok(_) => {
                                handle_clone.complete_with_result(content);
                            }
                            Err(e) => { handle_clone.complete_with_error(format!("Failed to read content: {}", e)); }
                        }
                    }
                    Ok(None) => {
                        // Entry not found - return empty
                        handle_clone.complete_with_result(Vec::<u8>::new());
                    }
                    Err(e) => { handle_clone.complete_with_error(format!("Failed to query entry: {}", e)); }
                }
            }
            Ok(None) => { handle_clone.complete_with_error("Document not found".to_string()); }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to open doc: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the value from a completed get_latest operation (same as iroh_docs_get_result)
#[no_mangle]
pub extern "C" fn iroh_docs_get_latest_result(handle: *mut IrohAsyncHandle, out_len: *mut usize) -> *mut u8 {
    iroh_docs_get_result(handle, out_len)
}

/// Key-value pair returned by get_all
#[repr(C)]
pub struct IrohKeyValuePair {
    pub key: *mut c_char,
    pub value: *mut u8,
    pub value_len: usize,
}

/// Get all latest entries from a document as key-value pairs
#[no_mangle]
pub extern "C" fn iroh_docs_get_all(
    docs: *const IrohDocs,
    namespace: *const IrohNamespaceId,
) -> *mut IrohAsyncHandle {
    if docs.is_null() || namespace.is_null() {
        set_last_error("NULL argument");
        return ptr::null_mut();
    }

    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);

    let docs_handle = unsafe { &(*docs) };
    let docs = docs_handle.docs.clone();
    let blobs_store = docs_handle.blobs_store.clone();
    let namespace = unsafe { (*namespace).0 };

    runtime.spawn(async move {
        match docs.open(namespace).await {
            Ok(Some(doc)) => {
                let query = iroh_docs::store::Query::single_latest_per_key().build();
                match doc.get_many(query).await {
                    Ok(stream) => {
                        use tokio::io::AsyncReadExt;
                        use n0_future::StreamExt;
                        let mut stream = std::pin::pin!(stream);
                        let mut pairs: Vec<(String, Vec<u8>)> = Vec::new();
                        while let Some(entry_result) = stream.next().await {
                            match entry_result {
                                Ok(entry) => {
                                    let key = String::from_utf8_lossy(entry.key()).to_string();
                                    let hash = entry.content_hash();
                                    match blobs_store.has(hash).await {
                                        Ok(true) => {
                                            let mut reader = blobs_store.reader(hash);
                                            let mut content = Vec::new();
                                            match reader.read_to_end(&mut content).await {
                                                Ok(_) => { pairs.push((key, content)); }
                                                Err(e) => {
                                                    handle_clone.complete_with_error(format!("Failed to read content for key '{}': {}", key, e));
                                                    return;
                                                }
                                            }
                                        }
                                        Ok(false) => {
                                            // Skip entries whose content hasn't been downloaded yet
                                        }
                                        Err(e) => {
                                            handle_clone.complete_with_error(format!("Failed to check content for key '{}': {}", key, e));
                                            return;
                                        }
                                    }
                                }
                                Err(e) => {
                                    handle_clone.complete_with_error(format!("Failed to read entry: {}", e));
                                    return;
                                }
                            }
                        }
                        handle_clone.complete_with_result(pairs);
                    }
                    Err(e) => { handle_clone.complete_with_error(format!("Failed to query entries: {}", e)); }
                }
            }
            Ok(None) => { handle_clone.complete_with_error("Document not found".to_string()); }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to open doc: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the result of a get_all operation — returns array of key-value pairs
#[no_mangle]
pub extern "C" fn iroh_docs_get_all_result(handle: *mut IrohAsyncHandle, out_count: *mut usize) -> *mut IrohKeyValuePair {
    if handle.is_null() { return ptr::null_mut(); }
    if !out_count.is_null() { unsafe { *out_count = 0; } }

    let pairs = unsafe { (*handle).take_result::<Vec<(String, Vec<u8>)>>() };
    match pairs {
        Some(pairs) => {
            let count = pairs.len();
            if count == 0 {
                if !out_count.is_null() { unsafe { *out_count = 0; } }
                return ptr::null_mut();
            }
            let mut c_pairs: Vec<IrohKeyValuePair> = Vec::with_capacity(count);
            for (key, value) in pairs {
                let c_key = CString::new(key).unwrap_or_default();
                let val_len = value.len();
                let val_ptr = if val_len > 0 {
                    let mut b = value.into_boxed_slice();
                    let p = b.as_mut_ptr();
                    std::mem::forget(b);
                    p
                } else {
                    ptr::null_mut()
                };
                c_pairs.push(IrohKeyValuePair {
                    key: c_key.into_raw(),
                    value: val_ptr,
                    value_len: val_len,
                });
            }
            let mut boxed = c_pairs.into_boxed_slice();
            let ptr = boxed.as_mut_ptr();
            std::mem::forget(boxed);
            if !out_count.is_null() { unsafe { *out_count = count; } }
            ptr
        }
        None => ptr::null_mut(),
    }
}

/// Free key-value pairs returned by iroh_docs_get_all_result
#[no_mangle]
pub extern "C" fn iroh_key_value_pairs_free(pairs: *mut IrohKeyValuePair, count: usize) {
    if pairs.is_null() || count == 0 { return; }
    unsafe {
        let slice = std::slice::from_raw_parts_mut(pairs, count);
        for pair in slice.iter_mut() {
            if !pair.key.is_null() { drop(CString::from_raw(pair.key)); }
            if !pair.value.is_null() && pair.value_len > 0 {
                let _ = Vec::from_raw_parts(pair.value, pair.value_len, pair.value_len);
            }
        }
        let _ = Box::from_raw(std::slice::from_raw_parts_mut(pairs, count) as *mut [IrohKeyValuePair]);
    }
}

/// Delete an entry from a document
#[no_mangle]
pub extern "C" fn iroh_docs_delete(
    docs: *const IrohDocs,
    namespace: *const IrohNamespaceId,
    author: *const IrohAuthorId,
    key: *const c_char,
) -> *mut IrohAsyncHandle {
    if docs.is_null() || namespace.is_null() || author.is_null() || key.is_null() { 
        set_last_error("NULL argument"); 
        return ptr::null_mut(); 
    }
    
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let docs = unsafe { (*docs).docs.clone() };
    let namespace = unsafe { (*namespace).0 };
    let author = unsafe { (*author).0 };
    let key_str = unsafe { CStr::from_ptr(key) };
    let key_bytes: Bytes = key_str.to_bytes().to_vec().into();
    
    runtime.spawn(async move {
        match docs.open(namespace).await {
            Ok(Some(doc)) => {
                match doc.del(author, key_bytes).await {
                    Ok(_) => {
                        handle_clone.complete_with_result(());
                    }
                    Err(e) => { handle_clone.complete_with_error(format!("Failed to delete entry: {}", e)); }
                }
            }
            Ok(None) => { handle_clone.complete_with_error("Document not found".to_string()); }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to open doc: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Check if a delete operation completed successfully
#[no_mangle]
pub extern "C" fn iroh_docs_delete_result(handle: *mut IrohAsyncHandle) -> IrohError {
    if handle.is_null() { return IrohError::InvalidArgument; }
    unsafe { (*handle).take_result::<()>().map(|_| IrohError::Ok).unwrap_or(IrohError::Pending) }
}

/// Create a ticket for sharing a document
#[no_mangle]
pub extern "C" fn iroh_docs_create_ticket(
    docs: *const IrohDocs,
    namespace: *const IrohNamespaceId,
    endpoint: *const IrohEndpoint,
) -> *mut IrohAsyncHandle {
    if docs.is_null() || namespace.is_null() || endpoint.is_null() { 
        set_last_error("NULL argument"); 
        return ptr::null_mut(); 
    }
    
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let docs = unsafe { (*docs).docs.clone() };
    let namespace = unsafe { (*namespace).0 };
    let _ep = unsafe { (*endpoint).0.clone() }; // Keep for potential future use
    
    runtime.spawn(async move {
        match docs.open(namespace).await {
            Ok(Some(doc)) => {
                match doc.share(ShareMode::Write, AddrInfoOptions::RelayAndAddresses).await {
                    Ok(ticket) => {
                        handle_clone.complete_with_result(IrohDocTicket(ticket));
                    }
                    Err(e) => { handle_clone.complete_with_error(format!("Failed to create ticket: {}", e)); }
                }
            }
            Ok(None) => { handle_clone.complete_with_error("Document not found".to_string()); }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to open doc: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the ticket from a completed create_ticket operation
#[no_mangle]
pub extern "C" fn iroh_docs_create_ticket_result(handle: *mut IrohAsyncHandle) -> *mut IrohDocTicket {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohDocTicket>().map(|t| Box::into_raw(Box::new(t))).unwrap_or(ptr::null_mut()) }
}

/// Convert a doc ticket to string
#[no_mangle]
pub extern "C" fn iroh_doc_ticket_to_string(ticket: *const IrohDocTicket) -> *mut c_char {
    if ticket.is_null() { return ptr::null_mut(); }
    CString::new(unsafe { &(*ticket).0 }.to_string()).map(|s| s.into_raw()).unwrap_or(ptr::null_mut())
}

/// Parse a doc ticket from string
#[no_mangle]
pub extern "C" fn iroh_doc_ticket_from_string(s: *const c_char) -> *mut IrohDocTicket {
    if s.is_null() { set_last_error("NULL string"); return ptr::null_mut(); }
    let s = unsafe { CStr::from_ptr(s) };
    let s = match s.to_str() { Ok(s) => s, Err(e) => { set_last_error(format!("Invalid UTF-8: {}", e)); return ptr::null_mut(); } };
    match s.parse::<DocTicket>() {
        Ok(ticket) => Box::into_raw(Box::new(IrohDocTicket(ticket))),
        Err(e) => { set_last_error(format!("Failed to parse doc ticket: {}", e)); ptr::null_mut() }
    }
}

/// Get the namespace ID from a doc ticket
#[no_mangle]
pub extern "C" fn iroh_doc_ticket_namespace(ticket: *const IrohDocTicket) -> *mut IrohNamespaceId {
    if ticket.is_null() { return ptr::null_mut(); }
    let capability = &unsafe { &(*ticket).0 }.capability;
    Box::into_raw(Box::new(IrohNamespaceId(capability.id())))
}

/// Free a doc ticket
#[no_mangle]
pub extern "C" fn iroh_doc_ticket_free(ticket: *mut IrohDocTicket) {
    if !ticket.is_null() { unsafe { drop(Box::from_raw(ticket)); } }
}

/// Join a document using a ticket (imports and syncs)
/// This function waits for the initial sync and content download to complete before returning
#[no_mangle]
pub extern "C" fn iroh_docs_join(
    docs: *const IrohDocs,
    ticket: *const IrohDocTicket,
) -> *mut IrohAsyncHandle {
    if docs.is_null() || ticket.is_null() { 
        set_last_error("NULL argument"); 
        return ptr::null_mut(); 
    }
    
    let runtime = match get_runtime() { Some(rt) => rt, None => { set_last_error("Runtime not initialized"); return ptr::null_mut(); } };
    let handle = match IrohAsyncHandle::new() { Ok(h) => h, Err(_) => { set_last_error("Failed to create async handle"); return ptr::null_mut(); } };
    let handle_clone = handle.clone();
    let handle_ptr = Arc::into_raw(handle);
    
    let docs = unsafe { (*docs).docs.clone() };
    let ticket = unsafe { (*ticket).0.clone() };
    
    runtime.spawn(async move {
        use n0_future::StreamExt;
        
        // Import and subscribe to events so we can wait for sync
        match docs.import_and_subscribe(ticket).await {
            Ok((doc, mut events)) => {
                let namespace_id = doc.id();
                tracing::info!("Joined document {}, waiting for sync events...", namespace_id);
                
                // Wait for sync and content to be ready (with timeout)
                let timeout = tokio::time::Duration::from_secs(60);
                let mut sync_finished = false;
                let mut content_ready = false;
                let mut entries_received = 0u64;
                
                let sync_result = tokio::time::timeout(timeout, async {
                    while let Some(event) = events.next().await {
                        match event {
                            Ok(iroh_docs::engine::LiveEvent::SyncFinished(ev)) => {
                                tracing::info!("SyncFinished event: {:?}", ev);
                                sync_finished = true;
                                // Wait a bit more for content to download
                                if entries_received == 0 {
                                    // No entries, we're done
                                    return true;
                                }
                                // If we already have content_ready, we're done
                                if content_ready {
                                    return true;
                                }
                                // Otherwise wait a bit more for content
                                continue;
                            }
                            Ok(iroh_docs::engine::LiveEvent::PendingContentReady) => {
                                tracing::info!("PendingContentReady event - all content downloaded");
                                content_ready = true;
                                // If sync finished, we're done
                                if sync_finished {
                                    return true;
                                }
                                continue;
                            }
                            Ok(iroh_docs::engine::LiveEvent::ContentReady { hash }) => {
                                tracing::info!("ContentReady event: hash={}", hash);
                                // Individual content piece is ready
                                // Continue waiting for more or PendingContentReady
                                continue;
                            }
                            Ok(iroh_docs::engine::LiveEvent::InsertRemote { entry, .. }) => {
                                entries_received += 1;
                                tracing::info!("InsertRemote event: key={:?}, total entries={}", 
                                    String::from_utf8_lossy(entry.key()), entries_received);
                                continue;
                            }
                            Ok(ev) => {
                                tracing::debug!("Other event: {:?}", ev);
                                continue;
                            }
                            Err(e) => {
                                tracing::warn!("Event error during sync: {}", e);
                                continue;
                            }
                        }
                    }
                    // Stream ended
                    tracing::info!("Event stream ended. sync_finished={}, content_ready={}, entries={}", 
                        sync_finished, content_ready, entries_received);
                    sync_finished
                }).await;
                
                match sync_result {
                    Ok(true) => {
                        tracing::info!("Sync complete");
                        handle_clone.complete_with_result(IrohNamespaceId(namespace_id));
                    }
                    Ok(false) => {
                        // Stream ended without sync completing, but doc is imported
                        tracing::warn!("Event stream ended, doc imported");
                        handle_clone.complete_with_result(IrohNamespaceId(namespace_id));
                    }
                    Err(_) => {
                        // Timeout - doc is imported but sync may not be complete
                        tracing::warn!("Sync timeout, doc imported but sync may be incomplete");
                        handle_clone.complete_with_result(IrohNamespaceId(namespace_id));
                    }
                }
            }
            Err(e) => { handle_clone.complete_with_error(format!("Failed to join doc: {}", e)); }
        }
    });
    handle_ptr as *mut IrohAsyncHandle
}

/// Get the namespace ID from a completed join operation
#[no_mangle]
pub extern "C" fn iroh_docs_join_result(handle: *mut IrohAsyncHandle) -> *mut IrohNamespaceId {
    if handle.is_null() { return ptr::null_mut(); }
    unsafe { (*handle).take_result::<IrohNamespaceId>().map(|n| Box::into_raw(Box::new(n))).unwrap_or(ptr::null_mut()) }
}

/// Free docs protocol
#[no_mangle]
pub extern "C" fn iroh_docs_free(docs: *mut IrohDocs) {
    if !docs.is_null() { unsafe { drop(Box::from_raw(docs)); } }
}

/// Get the ALPN for docs protocol
#[no_mangle]
pub extern "C" fn iroh_docs_alpn() -> *mut c_char {
    CString::new(iroh_docs::ALPN).map(|s| s.into_raw()).unwrap_or(ptr::null_mut())
}
