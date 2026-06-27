#include "Server.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

namespace {

std::string WideToUtf8Local(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWideLocal(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string UrlDecode(const std::string& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%' && i + 2 < v.size()) {
            char hex[3] = { v[i + 1], v[i + 2], 0 };
            char* end = nullptr;
            long c = strtol(hex, &end, 16);
            if (end && *end == 0) { out.push_back((char)c); i += 2; continue; }
        } else if (v[i] == '+') { out.push_back(' '); continue; }
        out.push_back(v[i]);
    }
    return out;
}

std::string QueryValue(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string part = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = part.find('=');
        std::string k = UrlDecode(eq == std::string::npos ? part : part.substr(0, eq));
        if (k == key) return UrlDecode(eq == std::string::npos ? std::string() : part.substr(eq + 1));
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return {};
}

std::string JsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", (unsigned)(unsigned char)c); o += b; }
            else o += c;
        }
    }
    return o;
}

bool SendAll(SOCKET s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int chunk = send(s, data + sent, (int)(std::min)(len - sent, (size_t)(64 * 1024)), 0);
        if (chunk <= 0) return false;
        sent += (size_t)chunk;
    }
    return true;
}

void SendResponse(SOCKET s, int status, const char* statusText, const std::string& contentType, const std::string& body) {
    std::ostringstream head;
    head << "HTTP/1.1 " << status << " " << statusText << "\r\n"
         << "Content-Type: " << contentType << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    std::string h = head.str();
    SendAll(s, h.data(), h.size());
    SendAll(s, body.data(), body.size());
}

void SendJson(SOCKET s, const std::string& json) { SendResponse(s, 200, "OK", "application/json; charset=utf-8", json); }
void SendOk(SOCKET s) { SendResponse(s, 200, "OK", "text/plain; charset=utf-8", "ok"); }
void SendErr(SOCKET s, int code, const char* text) { SendResponse(s, code, text, "text/plain; charset=utf-8", text); }

std::string FormatBytes(unsigned long long b) {
    if (b < 1024ull) return std::to_string(b) + " B";
    if (b < 1024ull * 1024ull) { char x[32]; sprintf_s(x, "%.1f KB", (double)b / 1024.0); return x; }
    char buf[32] = {};
    if (b < 1024ull * 1024ull * 1024ull) { sprintf_s(buf, "%.1f MB", (double)b / (1024.0 * 1024.0)); return buf; }
    sprintf_s(buf, "%.2f GB", (double)b / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

std::string FormatTime(const FILETIME& ft) {
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return {};
    char buf[32];
    sprintf_s(buf, "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

bool IsTextFile(const std::wstring& name) {
    std::wstring lo = name;
    std::transform(lo.begin(), lo.end(), lo.begin(), ::towlower);
    static const wchar_t* exts[] = {
        L".yml", L".yaml", L".json", L".properties", L".txt", L".log", L".toml",
        L".cfg", L".conf", L".ini", L".xml", L".md", L".csv", L".sh", L".bat",
        L".mcmeta", L".accesswidener", L".lang", L".css", L".js", L".html"
    };
    for (auto e : exts) {
        size_t n = wcslen(e);
        if (lo.size() >= n && lo.compare(lo.size() - n, n, e) == 0) return true;
    }
    if (lo == L"eula.txt" || lo.find(L'.') == std::wstring::npos) return false;
    return false;
}

}

namespace {

struct State {
    std::atomic<bool> running{ false };
    std::atomic<bool> stop{ false };
    std::atomic<SOCKET> listenSock{ INVALID_SOCKET };
    std::thread thread;
    std::wstring root;
    int port = 27633;
    std::function<bool()> statusProvider;
};
State g;

std::wstring SafeName(const std::wstring& n) {
    std::wstring out;
    for (wchar_t c : n) {
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
            c == L'"' || c == L'<' || c == L'>' || c == L'|') continue;
        out.push_back(c);
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    if (out == L"." || out == L"..") out.clear();
    return out;
}

bool ResolveUnderRoot(const std::string& relUtf8, std::wstring& outFull, std::wstring& outRelNorm) {
    std::wstring rel = Utf8ToWideLocal(relUtf8);
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : rel) {
        if (c == L'/' || c == L'\\') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(cur);

    std::wstring full = g.root;
    std::wstring norm;
    for (auto& p : parts) {
        if (p.empty() || p == L".") continue;
        if (p == L".." || p.find(L':') != std::wstring::npos) return false;
        full += L"\\" + p;
        if (!norm.empty()) norm += L"/";
        norm += p;
    }
    outFull = full;
    outRelNorm = norm;
    return true;
}

std::string LocalAddress() {
    char host[256] = {};
    if (gethostname(host, sizeof(host)) != 0) return "127.0.0.1";
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &result) != 0 || !result) return "127.0.0.1";
    std::string fallback = "127.0.0.1";
    for (addrinfo* p = result; p; p = p->ai_next) {
        sockaddr_in* sin = (sockaddr_in*)p->ai_addr;
        char ip[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) {
            std::string str = ip;
            if (str.rfind("127.", 0) != 0 && str.rfind("169.254.", 0) != 0) {
                freeaddrinfo(result);
                return str;
            }
            fallback = str;
        }
    }
    freeaddrinfo(result);
    return fallback;
}

void SendFile(SOCKET s, const std::wstring& path, const std::string& downloadName) {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) ||
        (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        SendErr(s, 404, "Not Found");
        return;
    }
    unsigned long long size = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) { SendErr(s, 404, "Not Found"); return; }
    std::ostringstream head;
    head << "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
         << "Content-Length: " << size << "\r\n"
         << "Content-Disposition: attachment; filename=\"" << downloadName << "\"\r\n"
         << "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    std::string h = head.str();
    if (!SendAll(s, h.data(), h.size())) { fclose(f); return; }
    std::vector<unsigned char> buf(256 * 1024);
    unsigned long long remaining = size;
    while (remaining > 0) {
        size_t want = (size_t)(std::min)(remaining, (unsigned long long)buf.size());
        size_t got = fread(buf.data(), 1, want, f);
        if (got == 0) break;
        if (!SendAll(s, (const char*)buf.data(), got)) break;
        remaining -= got;
    }
    fclose(f);
}

std::string ParseBoundary(const std::map<std::string, std::string>& headers) {
    auto it = headers.find("content-type");
    if (it == headers.end()) return {};
    size_t bpos = it->second.find("boundary=");
    if (bpos == std::string::npos) return {};
    std::string b = it->second.substr(bpos + 9);
    if (!b.empty() && b.front() == '"' && b.back() == '"') b = b.substr(1, b.size() - 2);
    return "--" + b;
}

bool ExtractFilePart(const std::string& boundary, const std::string& body,
                     std::wstring& fileName, const char*& dataPtr, size_t& dataLen) {
    if (boundary.empty()) return false;
    size_t pos = 0;
    while (true) {
        size_t partStart = body.find(boundary, pos);
        if (partStart == std::string::npos) return false;
        size_t headerStart = body.find("\r\n", partStart);
        if (headerStart == std::string::npos) return false;
        size_t headerEnd = body.find("\r\n\r\n", headerStart + 2);
        if (headerEnd == std::string::npos) return false;
        std::string ph = body.substr(headerStart + 2, headerEnd - headerStart - 2);
        size_t fn = ph.find("filename=\"");
        if (fn != std::string::npos) {
            fn += 10;
            size_t fnEnd = ph.find('"', fn);
            if (fnEnd == std::string::npos) return false;
            fileName = SafeName(Utf8ToWideLocal(ph.substr(fn, fnEnd - fn)));
            size_t dataStart = headerEnd + 4;
            size_t dataEnd = body.find("\r\n" + boundary, dataStart);
            if (dataEnd == std::string::npos || dataEnd < dataStart) return false;
            dataPtr = body.data() + dataStart;
            dataLen = dataEnd - dataStart;
            return !fileName.empty();
        }
        pos = headerEnd + 4;
    }
}

}

namespace {

const char* PageHtml() {
    return R"PAGE(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>JavaServerUWP Files</title>
<style>
:root{--bg:#0c1116;--bar:#121a22;--panel:#0f161d;--line:#22303c;--row:#0f161d;--rowh:#16222d;--muted:#8aa0b0;--text:#e8f0f5;--accent:#76c990;--danger:#ef7a6f;--blue:#5aa9e6}
*{box-sizing:border-box}html,body{margin:0;height:100%}
body{background:var(--bg);color:var(--text);font:14px/1.5 system-ui,Segoe UI,sans-serif}
.app{display:flex;flex-direction:column;height:100vh}
.top{display:flex;align-items:center;gap:14px;padding:12px 18px;background:var(--bar);border-bottom:1px solid var(--line)}
.top h1{font-size:16px;margin:0;font-weight:600}
.pill{font-size:12px;border:1px solid var(--line);border-radius:999px;padding:3px 10px;color:var(--muted)}
.pill.on{color:var(--accent);border-color:#2c4636}.pill.off{color:var(--danger);border-color:#4a2c2a}
.spacer{flex:1}
.btn{font:inherit;font-size:13px;border:1px solid var(--line);background:#16212b;color:var(--text);border-radius:8px;padding:7px 12px;cursor:pointer}
.btn:hover{background:#1b2935}.btn.primary{background:var(--accent);color:#08130c;border-color:transparent;font-weight:600}
.btn.blue{background:var(--blue);color:#06121c;border-color:transparent;font-weight:600}
.btn.danger{background:#3a1f1d;color:#ffd7d2;border-color:#5e3330}
.btn.ghost{background:transparent;border-color:transparent;color:var(--muted);padding:5px 8px}
.btn.ghost:hover{background:#1b2935;color:var(--text)}
.crumbs{display:flex;align-items:center;gap:4px;flex-wrap:wrap;padding:10px 18px;border-bottom:1px solid var(--line);background:var(--panel)}
.crumbs a{color:var(--blue);text-decoration:none;padding:2px 6px;border-radius:6px}.crumbs a:hover{background:#16222d}
.crumbs span.sep{color:var(--muted)}
.list{flex:1;overflow:auto}
.row{display:grid;grid-template-columns:1fr 120px 160px auto;align-items:center;gap:10px;padding:9px 18px;border-bottom:1px solid #18242e;cursor:default}
.row:hover{background:var(--rowh)}
.name{display:flex;align-items:center;gap:10px;min-width:0}
.name .nm{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;cursor:pointer}
.name .nm:hover{text-decoration:underline}
.ic{width:18px;height:18px;flex:0 0 18px;color:var(--muted)}.ic.dir{color:#e2b65c}
.size,.mod{color:var(--muted);font-size:12.5px;font-variant-numeric:tabular-nums}
.acts{display:flex;gap:4px;justify-content:flex-end;opacity:0;transition:opacity .1s}
.row:hover .acts{opacity:1}
.empty,.err{padding:40px 18px;color:var(--muted);text-align:center}
.drop{position:fixed;inset:0;background:rgba(12,17,22,.85);border:3px dashed var(--accent);display:none;align-items:center;justify-content:center;font-size:20px;color:var(--accent);z-index:50}
.drop.show{display:flex}
.editor{position:fixed;inset:0;background:var(--bg);display:none;flex-direction:column;z-index:40}
.editor.show{display:flex}
.ehead{display:flex;align-items:center;gap:10px;padding:10px 16px;background:var(--bar);border-bottom:1px solid var(--line)}
.ehead .path{font-family:Consolas,ui-monospace,monospace;color:var(--muted);font-size:13px;flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#ta{flex:1;width:100%;border:0;outline:0;resize:none;background:#070b0f;color:#dce7ef;font:13px/1.55 Consolas,ui-monospace,monospace;padding:14px;white-space:pre;tab-size:4}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#1b2935;border:1px solid var(--line);color:var(--text);padding:9px 16px;border-radius:10px;opacity:0;transition:opacity .2s;pointer-events:none;z-index:60}
.toast.show{opacity:1}
@media(max-width:680px){.row{grid-template-columns:1fr auto}.size,.mod{display:none}.acts{opacity:1}}
</style></head>
<body><div class="app">
<div class="top"><h1>JavaServerUWP</h1><span id="status" class="pill">...</span><span class="spacer"></span>
<button class="btn" onclick="mkdir()">New folder</button>
<button class="btn blue" onclick="document.getElementById('up').click()">Upload</button>
<button class="btn ghost" onclick="load(cur)" title="Refresh">Refresh</button>
<input id="up" type="file" multiple style="display:none" onchange="upload(this.files)"></div>
<div id="crumbs" class="crumbs"></div>
<div id="list" class="list"></div>
</div>
<div id="drop" class="drop">Drop files to upload</div>
<div id="editor" class="editor"><div class="ehead">
<span id="epath" class="path"></span>
<button class="btn primary" onclick="save()">Save</button>
<button class="btn" onclick="reloadEd()">Reload</button>
<button class="btn ghost" onclick="closeEd()">Close</button></div>
<textarea id="ta" spellcheck="false"></textarea></div>
<div id="toast" class="toast"></div>
<script>
var cur="",edPath="";
var ICON_DIR='<svg class="ic dir" viewBox="0 0 24 24" fill="currentColor"><path d="M10 4H4a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-8l-2-2z"/></svg>';
var ICON_FILE='<svg class="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><path d="M14 2v6h6"/></svg>';
function enc(p){return encodeURIComponent(p)}
function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;')}
function join(b,n){return b?b+"/"+n:n}
function parent(p){var i=p.lastIndexOf("/");return i<0?"":p.slice(0,i)}
function toast(m){var t=document.getElementById('toast');t.textContent=m;t.classList.add('show');clearTimeout(t._t);t._t=setTimeout(function(){t.classList.remove('show')},1800)}
function status(){fetch('/api/status').then(r=>r.json()).then(d=>{var s=document.getElementById('status');s.textContent=d.running?'RUNNING':'STOPPED';s.className='pill '+(d.running?'on':'off')}).catch(()=>{})}
async function load(path){
 cur=path||"";
 var r;try{r=await fetch('/api/list?path='+enc(cur))}catch(e){document.getElementById('list').innerHTML='<div class="err">Connection lost</div>';return}
 if(!r.ok){document.getElementById('list').innerHTML='<div class="err">Could not open folder</div>';return}
 render(await r.json());
}
function crumbs(){
 var c=document.getElementById('crumbs');var html='<a onclick="load(\'\')">root</a>';var acc="";
 if(cur){cur.split('/').forEach(function(seg){acc=join(acc,seg);html+='<span class="sep">/</span><a onclick="load(\''+acc.replace(/'/g,"\\'")+'\')">'+esc(seg)+'</a>'})}
 c.innerHTML=html;
}
function render(d){
 crumbs();
 var L=document.getElementById('list');
 if(!d.entries.length){L.innerHTML='<div class="empty">This folder is empty</div>';return}
 var h='';
 d.entries.forEach(function(e){
  var path=join(cur,e.name);var p=path.replace(/'/g,"\\'");
  var acts='';
  if(e.dir){
   acts='<button class="btn ghost" onclick="rename(\''+p+'\',\''+e.name.replace(/'/g,"\\'")+'\')">Rename</button>'
       +'<button class="btn ghost" onclick="del(\''+p+'\',1)">Delete</button>';
  }else{
   if(e.text)acts+='<button class="btn ghost" onclick="openEd(\''+p+'\')">Edit</button>';
   acts+='<button class="btn ghost" onclick="dl(\''+p+'\')">Download</button>'
       +'<button class="btn ghost" onclick="rename(\''+p+'\',\''+e.name.replace(/'/g,"\\'")+'\')">Rename</button>'
       +'<button class="btn ghost" onclick="del(\''+p+'\',0)">Delete</button>';
  }
  var click=e.dir?("load('"+p+"')"):(e.text?("openEd('"+p+"')"):("dl('"+p+"')"));
  h+='<div class="row"><div class="name">'+(e.dir?ICON_DIR:ICON_FILE)
    +'<span class="nm" onclick="'+click+'">'+esc(e.name)+'</span></div>'
    +'<div class="size">'+(e.dir?'':esc(e.sizeText))+'</div>'
    +'<div class="mod">'+esc(e.modified||'')+'</div>'
    +'<div class="acts">'+acts+'</div></div>';
 });
 L.innerHTML=h;
}
function dl(p){window.location='/download?path='+enc(p)}
async function del(p,isDir){
 if(!confirm('Delete '+(isDir?'folder':'file')+' "'+p.split('/').pop()+'"?'+(isDir?' This removes everything inside.':'')))return;
 await fetch('/api/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+enc(p)});
 toast('Deleted');load(cur);
}
async function rename(p,old){
 var nn=prompt('Rename to',old);if(!nn||nn===old)return;
 var r=await fetch('/api/rename',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+enc(p)+'&name='+enc(nn)});
 toast(r.ok?'Renamed':'Rename failed');load(cur);
}
async function mkdir(){
 var nn=prompt('New folder name');if(!nn)return;
 await fetch('/api/mkdir',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+enc(cur)+'&name='+enc(nn)});
 toast('Folder created');load(cur);
}
async function upload(files){
 if(!files.length)return;
 for(var i=0;i<files.length;i++){var fd=new FormData();fd.append('file',files[i]);
  await fetch('/api/upload?path='+enc(cur),{method:'POST',body:fd});}
 toast(files.length+' file(s) uploaded');document.getElementById('up').value='';load(cur);
}
function openEd(p){
 fetch('/raw?path='+enc(p)).then(r=>r.text()).then(function(t){
  edPath=p;document.getElementById('epath').textContent=p;document.getElementById('ta').value=t;
  document.getElementById('editor').classList.add('show');
 });
}
function reloadEd(){if(edPath)openEd(edPath)}
function closeEd(){document.getElementById('editor').classList.remove('show')}
async function save(){
 var r=await fetch('/api/write?path='+enc(edPath),{method:'POST',body:document.getElementById('ta').value});
 toast(r.ok?'Saved':'Save failed');
}
document.getElementById('ta').addEventListener('keydown',function(e){
 if(e.key==='Tab'){e.preventDefault();var t=this,s=t.selectionStart;t.value=t.value.slice(0,s)+'  '+t.value.slice(t.selectionEnd);t.selectionStart=t.selectionEnd=s+2}
 if((e.ctrlKey||e.metaKey)&&e.key==='s'){e.preventDefault();save()}
});
var dz=document.getElementById('drop');var dc=0;
window.addEventListener('dragenter',function(e){e.preventDefault();dc++;dz.classList.add('show')});
window.addEventListener('dragover',function(e){e.preventDefault()});
window.addEventListener('dragleave',function(e){dc--;if(dc<=0)dz.classList.remove('show')});
window.addEventListener('drop',function(e){e.preventDefault();dc=0;dz.classList.remove('show');if(e.dataTransfer.files.length)upload(e.dataTransfer.files)});
status();setInterval(status,5000);load('');
</script>
</body></html>)PAGE";
}

}

namespace {

bool DelTree(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(path.c_str()) != 0;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            DelTree(path + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(path.c_str()) != 0;
}

struct Entry {
    std::wstring name;
    bool dir;
    unsigned long long size;
    FILETIME mtime;
};

std::string ListJson(const std::string& query) {
    std::wstring full, rel;
    if (!ResolveUnderRoot(QueryValue(query, "path"), full, rel))
        return "{\"path\":\"\",\"entries\":[]}";

    std::vector<Entry> dirs, files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((full + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            Entry e;
            e.name = fd.cFileName;
            e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            e.mtime = fd.ftLastWriteTime;
            (e.dir ? dirs : files).push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    auto byName = [](const Entry& a, const Entry& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    std::ostringstream js;
    js << "{\"path\":\"" << JsonEsc(WideToUtf8Local(rel)) << "\",\"entries\":[";
    bool firstOut = true;
    auto emit = [&](const Entry& e) {
        if (!firstOut) js << ",";
        firstOut = false;
        js << "{\"name\":\"" << JsonEsc(WideToUtf8Local(e.name)) << "\","
           << "\"dir\":" << (e.dir ? "true" : "false") << ","
           << "\"size\":" << e.size << ","
           << "\"sizeText\":\"" << FormatBytes(e.size) << "\","
           << "\"modified\":\"" << FormatTime(e.mtime) << "\","
           << "\"text\":" << (!e.dir && IsTextFile(e.name) ? "true" : "false") << "}";
    };
    for (auto& e : dirs) emit(e);
    for (auto& e : files) emit(e);
    js << "]}";
    return js.str();
}

void HandleRaw(SOCKET s, const std::string& query) {
    std::wstring full, rel;
    if (!ResolveUnderRoot(QueryValue(query, "path"), full, rel) || rel.empty()) { SendErr(s, 400, "Bad Request"); return; }
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        SendErr(s, 404, "Not Found");
        return;
    }
    unsigned long long size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    if (size > 8ull * 1024ull * 1024ull) { SendErr(s, 413, "File too large to edit"); return; }
    FILE* f = nullptr;
    if (_wfopen_s(&f, full.c_str(), L"rb") != 0 || !f) { SendErr(s, 404, "Not Found"); return; }
    std::string data((size_t)size, 0);
    if (size > 0) fread(data.data(), 1, (size_t)size, f);
    fclose(f);
    SendResponse(s, 200, "OK", "text/plain; charset=utf-8", data);
}

void HandleWrite(SOCKET s, const std::string& query, const std::string& body) {
    std::wstring full, rel;
    if (!ResolveUnderRoot(QueryValue(query, "path"), full, rel) || rel.empty()) { SendErr(s, 400, "Bad Request"); return; }
    FILE* f = nullptr;
    if (_wfopen_s(&f, full.c_str(), L"wb") != 0 || !f) { SendErr(s, 500, "Write failed"); return; }
    if (!body.empty()) fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    SendOk(s);
}

void HandleRename(SOCKET s, const std::string& body) {
    std::wstring full, rel;
    std::wstring nn = SafeName(Utf8ToWideLocal(QueryValue(body, "name")));
    if (!ResolveUnderRoot(QueryValue(body, "path"), full, rel) || rel.empty() || nn.empty()) { SendErr(s, 400, "Bad Request"); return; }
    size_t slash = full.find_last_of(L'\\');
    std::wstring dest = full.substr(0, slash) + L"\\" + nn;
    if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) { SendErr(s, 409, "Name already exists"); return; }
    if (MoveFileExW(full.c_str(), dest.c_str(), 0)) SendOk(s);
    else SendErr(s, 500, "Rename failed");
}

void HandleDelete(SOCKET s, const std::string& body) {
    std::wstring full, rel;
    if (!ResolveUnderRoot(QueryValue(body, "path"), full, rel) || rel.empty()) { SendErr(s, 400, "Bad Request"); return; }
    if (DelTree(full)) SendOk(s);
    else SendErr(s, 500, "Delete failed");
}

void HandleMkdir(SOCKET s, const std::string& body) {
    std::wstring full, rel;
    std::wstring name = SafeName(Utf8ToWideLocal(QueryValue(body, "name")));
    if (!ResolveUnderRoot(QueryValue(body, "path"), full, rel) || name.empty()) { SendErr(s, 400, "Bad Request"); return; }
    if (CreateDirectoryW((full + L"\\" + name).c_str(), nullptr)) SendOk(s);
    else SendErr(s, 500, "Could not create folder");
}

void HandleUpload(SOCKET s, const std::string& query, const std::map<std::string, std::string>& headers, const std::string& body) {
    std::wstring full, rel;
    if (!ResolveUnderRoot(QueryValue(query, "path"), full, rel)) { SendErr(s, 400, "Bad Request"); return; }
    std::string boundary = ParseBoundary(headers);
    std::wstring fileName;
    const char* dataPtr = nullptr;
    size_t dataLen = 0;
    if (!ExtractFilePart(boundary, body, fileName, dataPtr, dataLen)) { SendErr(s, 400, "Upload failed"); return; }
    FILE* f = nullptr;
    if (_wfopen_s(&f, (full + L"\\" + fileName).c_str(), L"wb") != 0 || !f) { SendErr(s, 500, "Write failed"); return; }
    if (dataLen > 0) fwrite(dataPtr, 1, dataLen, f);
    fclose(f);
    SendOk(s);
}

bool ReadRequest(SOCKET s, std::string& request, std::string& body, std::map<std::string, std::string>& headers) {
    std::string data;
    char buffer[8192];
    size_t headerEnd = std::string::npos;
    while (data.size() < 1024 * 1024) {
        int read = recv(s, buffer, sizeof(buffer), 0);
        if (read <= 0) return false;
        data.append(buffer, read);
        headerEnd = data.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
    }
    if (headerEnd == std::string::npos) return false;
    request = data.substr(0, headerEnd);
    size_t lineStart = request.find("\r\n");
    size_t pos = lineStart == std::string::npos ? request.size() : lineStart + 2;
    while (pos < request.size()) {
        size_t next = request.find("\r\n", pos);
        std::string line = request.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)tolower(c); });
            size_t vs = colon + 1;
            while (vs < line.size() && line[vs] == ' ') ++vs;
            headers[key] = line.substr(vs);
        }
        if (next == std::string::npos) break;
        pos = next + 2;
    }
    unsigned long long contentLength = 0;
    auto it = headers.find("content-length");
    if (it != headers.end()) contentLength = strtoull(it->second.c_str(), nullptr, 10);
    if (contentLength > 512ull * 1024ull * 1024ull) return false;
    body = data.substr(headerEnd + 4);
    while (body.size() < contentLength) {
        int read = recv(s, buffer, sizeof(buffer), 0);
        if (read <= 0) return false;
        body.append(buffer, read);
    }
    if (body.size() > contentLength) body.resize((size_t)contentLength);
    return true;
}

void HandleClient(SOCKET s) {
    std::string request, body;
    std::map<std::string, std::string> headers;
    if (!ReadRequest(s, request, body, headers)) { SendErr(s, 400, "Bad Request"); return; }
    size_t firstEnd = request.find("\r\n");
    std::istringstream first(request.substr(0, firstEnd));
    std::string method, target, version;
    first >> method >> target >> version;
    size_t q = target.find('?');
    std::string path = q == std::string::npos ? target : target.substr(0, q);
    std::string query = q == std::string::npos ? std::string() : target.substr(q + 1);

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        SendResponse(s, 200, "OK", "text/html; charset=utf-8", PageHtml());
    } else if (method == "GET" && path == "/api/status") {
        bool running = g.statusProvider ? g.statusProvider() : false;
        SendJson(s, std::string("{\"running\":") + (running ? "true" : "false") + "}");
    } else if (method == "GET" && path == "/api/list") {
        SendJson(s, ListJson(query));
    } else if (method == "GET" && path == "/raw") {
        HandleRaw(s, query);
    } else if (method == "GET" && path == "/download") {
        std::wstring full, rel;
        if (!ResolveUnderRoot(QueryValue(query, "path"), full, rel) || rel.empty()) SendErr(s, 400, "Bad Request");
        else {
            size_t slash = rel.find_last_of(L'/');
            SendFile(s, full, WideToUtf8Local(slash == std::wstring::npos ? rel : rel.substr(slash + 1)));
        }
    } else if (method == "POST" && path == "/api/write") {
        HandleWrite(s, query, body);
    } else if (method == "POST" && path == "/api/rename") {
        HandleRename(s, body);
    } else if (method == "POST" && path == "/api/delete") {
        HandleDelete(s, body);
    } else if (method == "POST" && path == "/api/mkdir") {
        HandleMkdir(s, body);
    } else if (method == "POST" && path == "/api/upload") {
        HandleUpload(s, query, headers, body);
    } else {
        SendErr(s, 404, "Not Found");
    }
}

void WakeListener() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = htons((u_short)g.port);
    connect(s, (sockaddr*)&addr, sizeof(addr));
    closesocket(s);
}

void ThreadMain() {
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { g.running.store(false); return; }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { WSACleanup(); g.running.store(false); return; }
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((u_short)g.port);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(s, 8) != 0) {
        WriteLaunchLogF(L"WebPanel bind/listen failed port=%d err=%d", g.port, WSAGetLastError());
        closesocket(s);
        WSACleanup();
        g.running.store(false);
        return;
    }
    g.listenSock.store(s);
    WriteLaunchLogF(L"WebPanel listening on %ls", webpanel::Url().c_str());
    while (!g.stop.load()) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(s, &rs);
        timeval tv = {};
        tv.tv_usec = 250000;
        int ready = select(0, &rs, nullptr, nullptr, &tv);
        if (ready <= 0) continue;
        SOCKET client = accept(s, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        if (g.stop.load()) { closesocket(client); break; }
        HandleClient(client);
        closesocket(client);
    }
    SOCKET old = g.listenSock.exchange(INVALID_SOCKET);
    if (old != INVALID_SOCKET) closesocket(old);
    WSACleanup();
    WriteLaunchLog(L"WebPanel stopped");
}

}

namespace webpanel {

void SetStatusProvider(std::function<bool()> isRunning) { g.statusProvider = std::move(isRunning); }

void Start(const std::wstring& rootDir) {
    if (g.running.load()) return;
    if (g.thread.joinable()) g.thread.join();
    g.root = rootDir;
    g.stop.store(false);
    g.running.store(true);
    g.thread = std::thread(ThreadMain);
}

void Stop() {
    if (!g.running.load()) { if (g.thread.joinable()) g.thread.join(); return; }
    g.stop.store(true);
    WakeListener();
    if (g.thread.joinable()) g.thread.join();
    g.running.store(false);
}

bool Running() { return g.running.load(); }

std::wstring Url() {
    return L"http://" + Utf8ToWideLocal(LocalAddress()) + L":" + std::to_wstring(g.port);
}

}
