/* Minimal HTTP/1.0 server for a browser file manager.
 *   GET  /?p=DIR            folder listing page
 *   GET  /dl?p=FILE         download
 *   POST /up?p=DIR&n=NAME   upload (raw request body = file data)
 *   POST /mkdir?p=DIR&n=NAME
 *   POST /del?p=PATH        delete a file or an empty folder */
#include "web.h"
#include "apps.h"
#include "compat.h"
#include "config.h"
#include "dlog.h"
#include "fsops.h"
#include "hash.h"
#include "jb.h"
#include "payload.h"
#include "sysinfo.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HDR_MAX  8192
#define IO_BUF   (512 * 1024)
#define LOG_N    6
#define NET_SO_RCVTIMEO 0x1006          /* SceNet: int, microseconds */
#define NET_SO_SNDTIMEO 0x1005
#define RECV_TIMEOUT_US (60 * 1000000)  /* idle client: free its thread instead of waiting forever */
#define CONN_MAX 32                     /* connection threads at once; more get a 503 */

static volatile int running;
static volatile int nrequests;
static volatile int nconns;             /* live connection threads */
static int listen_sock = -1;
static pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;
static char log_ring[LOG_N][128];
static int log_head, log_count;

static void wlog(const char *fmt, ...)
{
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&log_mtx);
    snprintf(log_ring[log_head], sizeof log_ring[0], "%s", line);
    log_head = (log_head + 1) % LOG_N;
    if (log_count < LOG_N) log_count++;
    pthread_mutex_unlock(&log_mtx);
}

int web_log_line(int i, char *out, int n)
{
    int ok = 0;
    pthread_mutex_lock(&log_mtx);
    if (i < log_count) {
        snprintf(out, n, "%s", log_ring[(log_head - 1 - i + LOG_N) % LOG_N]);
        ok = 1;
    }
    pthread_mutex_unlock(&log_mtx);
    return ok;
}

int web_running(void) { return running; }
int web_requests(void) { return nrequests; }

/* ---------- socket helpers ---------- */
static int send_all(int s, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        int w = sceNetSend(s, p, len, 0);
        if (w <= 0) return -1;
        p += w;
        len -= w;
    }
    return 0;
}

static int sendf(int s, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (n < (int)sizeof buf) return send_all(s, buf, n);
    /* too long for the stack buffer (e.g. a row with long non-ASCII paths, URL-encoded several
       times): never cut it, or a half-written tag swallows the rest of the page */
    char *big = malloc((size_t)n + 1);
    if (!big) return -1;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int r = send_all(s, big, n);
    free(big);
    return r;
}

static void reply_text(int s, int code, const char *status, const char *msg)
{
    sendf(s, "HTTP/1.0 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %d\r\n"
             "Connection: close\r\n\r\n%s", code, status, (int)strlen(msg), msg);
}

/* ---------- string helpers ---------- */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *in, size_t len, char *out, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < len && k < n - 1; i++) {
        if (in[i] == '%' && i + 2 < len && hexval(in[i + 1]) >= 0 && hexval(in[i + 2]) >= 0) {
            out[k++] = (char)(hexval(in[i + 1]) * 16 + hexval(in[i + 2]));
            i += 2;
        } else if (in[i] == '+') {
            out[k++] = ' ';
        } else {
            out[k++] = in[i];
        }
    }
    out[k] = 0;
}

static void url_encode(const char *in, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && k + 4 < n; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~' || *p == '/') {
            out[k++] = *p;
        } else {
            out[k++] = '%';
            out[k++] = hex[*p >> 4];
            out[k++] = hex[*p & 15];
        }
    }
    out[k] = 0;
}

static void html_escape(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (const char *p = in; *p && k + 7 < n; p++) {
        const char *r = NULL;
        switch (*p) {
        case '&': r = "&amp;"; break;
        case '<': r = "&lt;"; break;
        case '>': r = "&gt;"; break;
        case '"': r = "&quot;"; break;
        case '\'': r = "&#39;"; break;
        }
        if (r) { size_t l = strlen(r); memcpy(out + k, r, l); k += l; }
        else out[k++] = *p;
    }
    out[k] = 0;
}

/* Gets query parameter `key` from "a=1&b=2" (decoded). */
static int get_param(const char *query, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    const char *p = query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            url_decode(p + kl + 1, len - kl - 1, out, n);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    out[0] = 0;
    return 0;
}

/* Canonical absolute path: collapses "//", resolves "." and "..", drops a trailing "/".
 * Every path from a request goes through this before any check (e.g. "/user/app/" or
 * "//user/app" must not slip past the protected-folder list). */
/* Returns -1 if the path can't be represented exactly (too deep / too long): the request is
 * then refused, rather than acting on a shortened - i.e. different - path. */
static int norm_path(char *p, size_t n)
{
    char tmp[1024], *parts[128];
    int np = 0;
    if (strlen(p) >= sizeof tmp) return -1;
    snprintf(tmp, sizeof tmp, "%s", p);
    char *save = NULL;
    for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (np) np--; continue; }
        if (np >= 128) return -1;
        parts[np++] = t;
    }
    if (!np) { snprintf(p, n, "/"); return 0; }
    char out[1024];
    size_t k = 0;
    for (int i = 0; i < np; i++) {
        int w = snprintf(out + k, sizeof out - k, "/%s", parts[i]);
        if (w < 0 || (size_t)w >= sizeof out - k) return -1;
        k += w;
    }
    if (k >= n) return -1;
    memcpy(p, out, k + 1);
    return 0;
}

static int valid_name(const char *n)
{
    for (const unsigned char *c = (const unsigned char *)n; *c; c++)
        if (*c < 32 || *c == 127) return 0;   /* CR/LF would break FTP listings and zip selections */
    return *n && !strchr(n, '/') && strcmp(n, ".") && strcmp(n, "..");
}

/* ---------- pages ---------- */
static const char *PAGE_HEAD =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>OrbisKit</title><style>"
    ":root{color-scheme:dark;--bg:#0f1318;--panel:#1a2129;--line:#2a333d;--acc:#3a7bd5;--txt:#e8ecef;"
    "--dim:#8995a1;--ok:#57c17b;--warn:#ffc857;--bad:#e5534b}"
    "*{box-sizing:border-box}body{margin:0;font:15px system-ui,sans-serif;background:var(--bg);color:var(--txt)}"
    /* sticky top bar: title + menu, then the page's own header content */
    "header{position:sticky;top:0;z-index:5;background:var(--panel);padding:10px 16px;display:flex;gap:10px 16px;"
    "align-items:center;flex-wrap:wrap;border-bottom:1px solid var(--line)}"
    "h1{font-size:17px;margin:0}nav{display:flex;gap:4px;flex-wrap:wrap}"
    "nav a{padding:6px 12px;border-radius:6px;color:var(--dim);font-weight:600}"
    "nav a:hover{background:#222a33;text-decoration:none}nav a.on{background:var(--acc);color:#fff}"
    "a{color:#6ea8ff;text-decoration:none}a:hover{text-decoration:underline}"
    ".crumb{font-family:monospace;word-break:break-all;flex-basis:100%}"
    "main{padding:14px 16px;max-width:1400px;margin:0 auto}"
    "table{width:100%;border-collapse:collapse}td{padding:8px 6px;border-bottom:1px solid #222a33}"
    "td.s{text-align:right;color:var(--dim);white-space:nowrap;font-family:monospace}"
    "button,label.b{background:var(--acc);color:#fff;border:0;border-radius:6px;padding:7px 12px;cursor:pointer;"
    "font:inherit;display:inline-block}"
    "button.d{background:transparent;color:var(--bad);padding:2px 8px}button.d.e{color:var(--dim)}"
    "input[type=search],input.t{font:inherit;padding:7px 10px;background:var(--panel);color:var(--txt);"
    "border:1px solid var(--line);border-radius:6px}"
    ".tools{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-bottom:10px}.tools .sp{flex:1}"
    "#selbar{position:sticky;top:0;background:var(--bg);padding:6px 0}"
    "#bar{height:6px;background:var(--line);margin-top:10px;display:none;border-radius:3px;overflow:hidden}"
    "#bar div{height:100%;width:0;background:var(--acc)}"
    "#msg{color:var(--dim);margin:6px 0;min-height:1em}"
    ".drop{outline:3px dashed var(--acc);outline-offset:-10px}"
    "img.ic{width:56px;height:56px;border-radius:6px;vertical-align:middle;margin-right:10px;background:#222a33}"
    /* dashboard */
    ".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:14px}"
    ".card{background:var(--panel);border-radius:10px;padding:14px 16px;border-top:3px solid var(--acc)}"
    ".card h3{margin:0 0 10px;font-size:15px;color:#6ea8ff}"
    ".kv{display:flex;justify-content:space-between;gap:10px;padding:4px 0}.kv span:first-child{color:var(--dim)}"
    ".meter{height:8px;background:var(--line);border-radius:4px;overflow:hidden;margin:2px 0 8px}"
    ".meter div{height:100%;background:var(--acc)}"
    ".tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin-top:18px}"
    ".tile{background:var(--panel);border-radius:10px;padding:18px 16px;color:var(--txt);display:block}"
    ".tile:hover{background:#222a33;text-decoration:none}.tile b{display:block;font-size:17px;margin-bottom:4px}"
    ".tile span{color:var(--dim);font-size:13px}"
    ".ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}.dim{color:var(--dim)}"
    "pre{background:var(--panel);padding:12px;border-radius:8px;overflow:auto;white-space:pre-wrap;font-size:13px}"
    /* phones: drop the date column so names have room */
    "@media(max-width:700px){.dt{display:none}main{padding:10px}header{padding:8px 10px}h1{font-size:15px}}"
    "</style></head><body>";

/* Extra text formats worth editing that we don't serve as text/plain for viewing. */
static int is_config_name(const char *name)
{
    static const char *ext[] = { ".conf", ".yml", ".yaml", ".csv", ".shn", ".mc4", ".lua", ".sh", ".c", ".h" };
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (unsigned i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (!strcasecmp(dot, ext[i])) return 1;
    return 0;
}

/* Top bar shared by every page; `active` highlights the current section. */
static void send_nav(int s, const char *active)
{
    static const struct { const char *id, *href, *label; } items[] = {
        { "home", "/", "Home" }, { "files", "/?p=/", "Files" }, { "apps", "/apps", "Apps" },
        { "gallery", "/gallery", "Gallery" }, { "payload", "/payload", "Payload" },
        { "settings", "/settings", "Settings" }, { "log", "/log", "Log" },
    };
    sendf(s, "<header><h1>OrbisKit</h1><nav>");
    for (unsigned i = 0; i < sizeof items / sizeof items[0]; i++)
        sendf(s, "<a href='%s'%s>%s</a>", items[i].href, !strcmp(items[i].id, active) ? " class=on" : "", items[i].label);
    sendf(s, "</nav>");
}

/* MIME type for viewing in the browser, or NULL to force a download. */
static const char *view_mime(const char *name)
{
    static const struct { const char *ext, *mime; } m[] = {
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" },
        { ".bmp", "image/bmp" }, { ".webp", "image/webp" },
        { ".mp4", "video/mp4" }, { ".m4v", "video/mp4" }, { ".webm", "video/webm" },
        { ".mp3", "audio/mpeg" }, { ".wav", "audio/wav" }, { ".ogg", "audio/ogg" }, { ".m4a", "audio/mp4" },
        { ".pdf", "application/pdf" },
        /* text types; .html etc. are deliberately shown as plain text, never run */
        { ".txt", "text/plain; charset=utf-8" }, { ".log", "text/plain; charset=utf-8" },
        { ".ini", "text/plain; charset=utf-8" }, { ".cfg", "text/plain; charset=utf-8" },
        { ".json", "text/plain; charset=utf-8" }, { ".xml", "text/plain; charset=utf-8" },
        { ".md", "text/plain; charset=utf-8" }, { ".html", "text/plain; charset=utf-8" },
        { ".js", "text/plain; charset=utf-8" }, { ".sfo", NULL },
    };
    const char *dot = strrchr(name, '.');
    if (!dot) return NULL;
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++)
        if (!strcasecmp(dot, m[i].ext)) return m[i].mime;
    return NULL;
}

static void send_listing(int s, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        reply_text(s, 404, "Not Found", "Folder not found or not accessible.");
        return;
    }
    sendf(s, "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
             "Connection: close\r\n\r\n");
    send_all(s, PAGE_HEAD, strlen(PAGE_HEAD));

    char enc[3072], esc[3072];
    url_encode(dir, enc, sizeof enc);

    /* header with clickable breadcrumb */
    send_nav(s, "files");
    sendf(s, "<span class=crumb><a href='/?p=/'>/</a>");
    char acc[1024] = "";
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", dir);
    char *save = NULL;
    for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        strncat(acc, "/", sizeof acc - strlen(acc) - 1);
        strncat(acc, t, sizeof acc - strlen(acc) - 1);
        char e1[3072], e2[1024];
        url_encode(acc, e1, sizeof e1);
        html_escape(t, e2, sizeof e2);
        sendf(s, "<a href='/?p=%s'>%s</a>/", e1, e2);
    }
    char desc[1100];
    html_escape(dir, desc, sizeof desc);
    unsigned long long dtot = 0, dfree = 0;
    char freebuf[48] = "";
    if (disk_space(dir, &dtot, &dfree) == 0) {
        char a[32];
        fmt_size(a, sizeof a, dfree);
        snprintf(freebuf, sizeof freebuf, "%s free", a);
    }
    sendf(s, "</span></header><main>"
             "<div class=tools>"
             "<label class=b>&#11014; Upload files<input id=f type=file multiple hidden></label>"
             "<label class=b>&#128193; Upload folder<input id=fd type=file webkitdirectory multiple hidden></label>"
             "<button onclick=mk()>+ New folder</button>"
             "<span class=sp></span>"
             "<form action=/search><input type=hidden name=p value=\"%s\">"
             "<input type=search name=q placeholder='Search in this folder'></form>"
             "</div>"
             "<div class='tools dim' style='font-size:13px'>Tip: drop files or whole folders anywhere on this page"
             "<span class=sp></span>%s</div>"
             "<div id=selbar style='display:none' class=tools><b id=selc></b> selected"
             "<button onclick=zipSel()>Download as .zip</button><button onclick=delSel()>Delete</button>"
             "<button onclick=clearSel()>Clear</button></div>"
             "<div id=bar><div></div></div><div id=msg></div><table>", desc, freebuf);

    if (strcmp(dir, "/")) {
        char up[1024];
        snprintf(up, sizeof up, "%s", dir);
        char *sl = strrchr(up, '/');
        if (sl == up) up[1] = 0; else if (sl) *sl = 0;
        url_encode(up, enc, sizeof enc);
        sendf(s, "<tr><td><a href='/?p=%s'>.. (up)</a></td><td></td><td></td><td></td></tr>", enc);
        url_encode(dir, enc, sizeof enc);
    }

    /* two passes: folders first, then files */
    struct dirent *e;
    char full[1024], sz[32];
    for (int pass = 0; pass < 2; pass++) {
        rewinddir(d);
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            /* a truncated path names a different item: its Delete/Rename buttons would hit that one */
            if (path_join(full, sizeof full, dir, e->d_name)) continue;
            struct kstat st;
            if (k_stat(full, &st) != 0) continue;
            int isdir = S_ISDIR(st.st_mode);
            if (isdir != (pass == 0)) continue;
            url_encode(full, enc, sizeof enc);
            html_escape(e->d_name, esc, sizeof esc);
            char date[24];
            time_t mt = st.st_mtime;
            struct tm tm;
            localtime_r(&mt, &tm);
            strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tm);
            char nenc[1024];
            url_encode(e->d_name, nenc, sizeof nenc);
            sendf(s, "<tr><td><input type=checkbox class=sel value='%s'> ", nenc);
            if (isdir) {
                sendf(s, "&#128193; <a href='/?p=%s'>%s</a></td><td class='s dt'>%s</td><td class=s>"
                         "<a href='/zip?p=%s' title='Download as .zip'>zip</a></td>", enc, esc, date, enc);
            } else {
                fmt_size(sz, sizeof sz, st.st_size);
                /* viewable files open in the browser; the arrow always downloads */
                const char *mime = view_mime(e->d_name);
                const char *route = mime ? "view" : "dl";
                int editable = (mime && !strncmp(mime, "text/", 5)) || is_config_name(e->d_name);
                sendf(s, "&#128196; <a href='/%s?p=%s' target=_blank>%s</a> "
                         "<a href='/dl?p=%s' title=Download>&#11015;</a>%s%s%s</td><td class='s dt'>%s</td><td class=s>%s</td>",
                      route, enc, esc, enc,
                      editable ? " <a href='/edit?p=" : "", editable ? enc : "", editable ? "' title=Edit>edit</a>" : "",
                      date, sz);
            }
            sendf(s, "<td class=s><button class='d e' title=Rename onclick=\"ren('%s')\">&#9998;</button>"
                     "<button class=d title=Delete onclick=\"del('%s',%d)\">&#10005;</button></td></tr>",
                  enc, enc, isdir);
        }
    }
    closedir(d);

    url_encode(dir, enc, sizeof enc);
    sendf(s, "</table></main><script>const P='%s';", enc);
    static const char *JS =
        "const f=document.getElementById('f'),bar=document.getElementById('bar'),"
        "fill=bar.firstChild,msg=document.getElementById('msg');"
        "function post(u){return fetch(u,{method:'POST',headers:{'X-TBox':'1'}}).then(r=>r.text().then(t=>{if(!r.ok)throw t;return t}))}"
        "function mk(){const n=prompt('Folder name');if(n)post('/mkdir?p='+P+'&n='+encodeURIComponent(n))"
        ".then(()=>location.reload(),e=>alert(e))}"
        "function del(p,d){if(confirm('Delete '+decodeURIComponent(p)+(d?' and everything inside it':'')+'?'))"
        "post('/del?p='+p).then(()=>location.reload(),e=>alert(e))}"
        "function ren(p){const old=decodeURIComponent(p).split('/').pop(),n=prompt('New name',old);"
        "if(n&&n!==old)post('/rename?p='+p+'&n='+encodeURIComponent(n)).then(()=>location.reload(),e=>alert(e))}"
        /* uploads: items are {file, dir} where dir is a relative sub-folder ('' = here) */
        "function up(it){return new Promise((ok,bad)=>{const x=new XMLHttpRequest();"
        "x.open('POST','/up?p='+P+'&n='+encodeURIComponent(it.file.name)+(it.dir?'&d='+encodeURIComponent(it.dir):''));"
        "x.setRequestHeader('X-TBox','1');"
        "x.upload.onprogress=e=>{if(e.lengthComputable)fill.style.width=(100*e.loaded/e.total)+'%'};"
        "x.onload=()=>x.status==200?ok():bad(x.responseText);x.onerror=()=>bad('Connection lost');x.send(it.file)})}"
        "async function upAll(items){if(!items.length)return;bar.style.display='block';let i=0;"
        "for(const it of items){i++;msg.textContent='Uploading '+(it.dir?it.dir+'/':'')+it.file.name+' ('+i+'/'+items.length+')';"
        "fill.style.width='0';try{await up(it)}catch(e){alert(it.file.name+': '+e);break}}location.reload()}"
        "const plain=fs=>[...fs].map(x=>({file:x,dir:''}));"
        "f.onchange=()=>upAll(plain(f.files));"
        "document.getElementById('fd').onchange=e=>upAll([...e.target.files].map(x=>{"
        "const r=x.webkitRelativePath||x.name;return{file:x,dir:r.split('/').slice(0,-1).join('/')}}));"
        /* drag & drop, including whole folders */
        "async function walk(en,path,out){if(en.isFile){out.push({file:await new Promise((a,b)=>en.file(a,b)),dir:path})}"
        "else if(en.isDirectory){const r=en.createReader(),p=path?path+'/'+en.name:en.name;let batch;"
        "do{batch=await new Promise((a,b)=>r.readEntries(a,b));for(const c of batch)await walk(c,p,out)}while(batch.length)}}"
        "document.body.ondragover=e=>{e.preventDefault();document.body.classList.add('drop')};"
        "document.body.ondragleave=e=>{if(!e.relatedTarget)document.body.classList.remove('drop')};"
        "document.body.ondrop=async e=>{e.preventDefault();document.body.classList.remove('drop');"
        "const ens=[...e.dataTransfer.items].map(i=>i.webkitGetAsEntry&&i.webkitGetAsEntry()).filter(Boolean);"
        "if(!ens.length){upAll(plain(e.dataTransfer.files));return}"
        "msg.textContent='Reading dropped items...';const out=[];for(const en of ens)await walk(en,'',out);upAll(out)};"
        /* multi-select */
        "const sels=[...document.querySelectorAll('.sel')],selbar=document.getElementById('selbar'),"
        "selc=document.getElementById('selc');"
        "function picked(){return sels.filter(c=>c.checked).map(c=>c.value)}"
        "function upd(){const n=picked().length;selbar.style.display=n?'flex':'none';selc.textContent=n}"
        "sels.forEach(c=>c.onchange=upd);"
        "function clearSel(){sels.forEach(c=>c.checked=false);upd()}"
        "const base=P==='/'?'':P;"
        "async function delSel(){const ns=picked();"
        "if(!confirm('Delete '+ns.length+' item(s)? Folders are deleted with everything inside.'))return;"
        "for(const n of ns){msg.textContent='Deleting '+decodeURIComponent(n);"
        "try{await post('/del?p='+base+'/'+n)}catch(e){alert(decodeURIComponent(n)+': '+e)}}location.reload()}"
        "async function zipSel(){const ns=picked().map(decodeURIComponent);msg.textContent='Zipping on the PS4...';"
        "try{const r=await fetch('/zipsel?p='+P,{method:'POST',headers:{'X-TBox':'1'},body:ns.join('\\n')});"
        "if(!r.ok){alert(await r.text());msg.textContent='';return}"
        "const cd=r.headers.get('Content-Disposition')||'',m=cd.match(/filename=\"([^\"]+)\"/);"
        "const a=document.createElement('a');a.href=URL.createObjectURL(await r.blob());"
        "a.download=m?m[1]:'selection.zip';document.body.appendChild(a);a.click();a.remove();msg.textContent=''}"
        "catch(e){alert('Connection lost');msg.textContent=''}}"
        "</script></body></html>";
    send_all(s, JS, strlen(JS));
}

/* rm -rf (the web server's own; doesn't touch the UI job). Never leaves the filesystem it
 * started on (a mounted USB drive or game image inside the folder is left alone), and stops
 * at a depth limit so a very deep tree can't overflow the thread's stack. */
#define REMOVE_DEPTH_MAX 128
static int remove_tree_dev(const char *path, uint64_t dev, int depth)
{
    struct kstat st;
    if (k_lstat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    if ((uint64_t)st.st_dev != dev) { errno = EXDEV; return -1; }   /* mount point: skip */
    if (depth >= REMOVE_DEPTH_MAX) { errno = ENAMETOOLONG; return -1; }
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        char sub[1024];
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (path_join(sub, sizeof sub, path, e->d_name)) continue;   /* never delete a truncated path */
            remove_tree_dev(sub, dev, depth + 1);
        }
        closedir(d);
    }
    return rmdir(path);
}

static int remove_tree(const char *path)
{
    struct kstat st;
    if (k_lstat(path, &st) != 0) return -1;
    if (is_mount_point(path)) { errno = EBUSY; return -1; }   /* never empty a whole drive */
    return remove_tree_dev(path, (uint64_t)st.st_dev, 0);
}

/* Paths that must never be deleted from the browser. */
static int protected_path(const char *p)
{
    static const char *keep[] = { "/", "/data", "/user", "/system", "/system_ex", "/system_data",
                                  "/mnt", "/dev", "/preinst", "/preinst2", "/update", "/user/app",
                                  "/user/home", "/user/patch", "/user/appmeta", "/user/addcont",
                                  "/user/license", "/user/system", "/system_data/priv",
                                  "/system_data/priv/mms", "/system_data/priv/appmeta", "/mnt/sandbox" };
    for (unsigned i = 0; i < sizeof keep / sizeof keep[0]; i++)
        if (!strcmp(p, keep[i])) return 1;
    return 0;
}

static void send_file(int s, const char *path);

/* Opens a regular file to stream. Returns the fd (st filled from it), -1 if it can't be
 * opened, -2 if it isn't a regular file: a FIFO would block open() and /dev/random would
 * stream forever, so those are checked before opening and again on the open fd. */
static int open_regular(const char *path, struct kstat *st)
{
    if (k_stat(path, st) != 0) return -1;
    if (!S_ISREG(st->st_mode)) return -2;
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    if (k_fstat(fd, st) != 0 || !S_ISREG(st->st_mode)) { close(fd); return -2; }
    return fd;
}

/* Zips src[] with the job thread, streams the archive as `zipname`, then deletes the temp file. */
static void zip_and_send(int s, char **src, int n, const char *zipname)
{
    /* a unique temp name per request: two downloads must not share (and corrupt) one file */
    static volatile int seq;
    char tmp[96];
    snprintf(tmp, sizeof tmp, "/data/OrbisKit/web_download_%d.zip", __sync_add_and_fetch(&seq, 1));
    mkdirs("/data/OrbisKit");
    if (job_start_quiet(JOB_ZIP, src, n, tmp) != 0) {
        reply_text(s, 503, "Busy", "The console is busy with another job. Try again in a moment.");
        return;
    }
    while (g_job.running) sceKernelUsleep(100000);
    /* files that couldn't be read are skipped */
    struct kstat st;
    int fd = open_regular(tmp, &st);
    if (fd < 0) {
        unlink(tmp);                       /* don't leave a partial archive behind */
        reply_text(s, 500, "Error", "Zip failed.");
        return;
    }
    char name[300];
    snprintf(name, sizeof name, "%s", zipname);
    for (char *c = name; *c; c++) if (*c == '"' || *c < 32) *c = '_';
    sendf(s, "HTTP/1.0 200 OK\r\nContent-Type: application/zip\r\nContent-Length: %llu\r\n"
             "Content-Disposition: attachment; filename=\"%s\"\r\nConnection: close\r\n\r\n",
          (unsigned long long)st.st_size, name);
    char *buf = malloc(IO_BUF);
    ssize_t r;
    while (buf && (r = read(fd, buf, IO_BUF)) > 0)
        if (send_all(s, buf, r)) break;
    free(buf);
    close(fd);
    unlink(tmp);
    wlog("Zipped and sent %s", name);
}

static void send_zip(int s, const char *dir)
{
    if (!is_dir(dir)) { reply_text(s, 404, "Not Found", "Folder not found."); return; }
    char name[300];
    snprintf(name, sizeof name, "%s.zip", strcmp(dir, "/") ? path_base(dir) : "root");
    char *src[1] = { (char *)dir };
    zip_and_send(s, src, 1, name);
}

/* POST /zipsel?p=DIR  body: one item name per line -> one .zip of those items */
static void receive_zipsel(int s, const char *dir, long long clen, const char *pre, int prelen)
{
    if (clen <= 0 || clen > 65536) { reply_text(s, 400, "Bad Request", "Nothing selected."); return; }
    char *body = malloc(clen + 1);
    if (!body) { reply_text(s, 500, "Error", "Out of memory."); return; }
    long long got = prelen > clen ? clen : prelen;
    if (got > 0) memcpy(body, pre, got);
    while (got < clen) {
        int r = sceNetRecv(s, body + got, clen - got, 0);
        if (r <= 0) break;
        got += r;
    }
    body[got] = 0;
    char *src[512];
    int n = 0;
    char *save = NULL;
    for (char *line = strtok_r(body, "\n", &save); line && n < 512; line = strtok_r(NULL, "\n", &save)) {
        line[strcspn(line, "\r")] = 0;
        if (!valid_name(line)) continue;
        char full[1024];
        if (path_join(full, sizeof full, dir, line)) continue;   /* truncated = a different item */
        struct kstat st;
        /* files and folders only: a FIFO or device node would hang or never end the zip */
        if (k_stat(full, &st) == 0 && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) &&
            (src[n] = strdup(full))) n++;
    }
    free(body);
    if (!n) { reply_text(s, 400, "Bad Request", "None of the selected items exist."); return; }
    char name[300];
    if (n == 1) snprintf(name, sizeof name, "%s.zip", path_base(src[0]));
    else snprintf(name, sizeof name, "%s (%d items).zip", strcmp(dir, "/") ? path_base(dir) : "root", n);
    zip_and_send(s, src, n, name);
    for (int i = 0; i < n; i++) free(src[i]);
}

/* ---------- search (the web server's own walk; doesn't use the TV's job) ---------- */
#define WEB_SEARCH_MAX 500

static const char *ci_find(const char *h, const char *n)
{
    size_t nl = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, nl)) return h;
    return NULL;
}

/* One result row (kept out of the recursive walk so its big buffers aren't on every level). */
static void search_emit(int s, const char *dir, const char *full, const char *name, int isdir, uint64_t size)
{
    char enc[3072], esc[600], penc[3072], pesc[1100], sz[32] = "";
    url_encode(full, enc, sizeof enc);
    html_escape(name, esc, sizeof esc);
    url_encode(dir, penc, sizeof penc);
    html_escape(dir, pesc, sizeof pesc);
    if (isdir) {
        sendf(s, "<tr><td>&#128193; <a href='/?p=%s'>%s</a></td><td class=crumb><a href='/?p=%s'>%s</a></td>"
                 "<td class=s></td></tr>", enc, esc, penc, pesc);
    } else {
        fmt_size(sz, sizeof sz, size);
        sendf(s, "<tr><td>&#128196; <a href='/%s?p=%s' target=_blank>%s</a></td><td class=crumb>"
                 "<a href='/?p=%s'>%s</a></td><td class=s>%s</td></tr>",
              view_mime(name) ? "view" : "dl", enc, esc, penc, pesc, sz);
    }
}

static void search_walk(int s, const char *dir, const char *q, int *found, int *checked, int depth)
{
    if (*found >= WEB_SEARCH_MAX || depth > 32 || !strcmp(dir, "/dev") || !strcmp(dir, "/proc")) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char full[1024];
    while ((e = readdir(d)) && *found < WEB_SEARCH_MAX) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (path_join(full, sizeof full, dir, e->d_name)) continue;   /* truncated = a parent's name */
        struct kstat st;
        if (k_lstat(full, &st) != 0) continue;
        (*checked)++;
        int isdir = S_ISDIR(st.st_mode);
        if (ci_find(e->d_name, q)) {
            search_emit(s, dir, full, e->d_name, isdir, st.st_size);
            (*found)++;
        }
        if (isdir) search_walk(s, full, q, found, checked, depth + 1);
    }
    closedir(d);
}

static void send_search(int s, const char *dir, const char *q)
{
    sendf(s, "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
             "Connection: close\r\n\r\n");
    send_all(s, PAGE_HEAD, strlen(PAGE_HEAD));
    send_nav(s, "files");
    char qesc[400], desc[1100], denc[3072];
    html_escape(q, qesc, sizeof qesc);
    html_escape(dir, desc, sizeof desc);
    url_encode(dir, denc, sizeof denc);
    sendf(s, "</header><main><form action=/search><input type=hidden name=p value=\"%s\">"
             "<input name=q value=\"%s\" autofocus style='font:inherit;padding:7px;width:min(400px,60vw);"
             "background:#1a2129;color:#e8ecef;border:1px solid #2a333d'> <button>Search</button> "
             "<a href='/?p=%s'>Back to %s</a></form>", desc, qesc, denc, desc);
    if (!q[0]) { sendf(s, "</main></body></html>"); return; }
    sendf(s, "<h3>Results for \"%s\" in %s</h3><table>", qesc, desc);
    int found = 0, checked = 0;
    search_walk(s, dir, q, &found, &checked, 0);
    sendf(s, "</table><p style='color:#8995a1'>%d match%s, %d items checked%s</p></main></body></html>",
          found, found == 1 ? "" : "es", checked, found >= WEB_SEARCH_MAX ? " (stopped at 500 results)" : "");
}

/* Opens a file inline in the browser, with Range support so videos can seek. */
static void send_view(int s, const char *path, const char *range)
{
    const char *mime = view_mime(path);
    if (!mime) { send_file(s, path); return; }
    struct kstat st;
    int fd = open_regular(path, &st);
    if (fd == -2) { reply_text(s, 403, "Forbidden", "Not a regular file."); return; }
    if (fd < 0) { reply_text(s, 404, "Not Found", "File not found."); return; }
    long long size = st.st_size, start = 0, end = size - 1;
    int partial = 0;
    long long suffix;
    if (range[0] && !strncmp(range, "bytes=-", 7) && sscanf(range + 7, "%lld", &suffix) == 1 && suffix > 0) {
        /* "bytes=-N" = the last N bytes (players use it to read an MP4 index at the end) */
        partial = 1;
        start = suffix >= size ? 0 : size - suffix;
        end = size - 1;
    } else if (range[0] && sscanf(range, "bytes=%lld-%lld", &start, &end) >= 1) {
        partial = 1;
        if (end >= size) end = size - 1;
        if (start < 0 || start > end) {
            sendf(s, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%lld\r\n"
                     "Content-Length: 0\r\nConnection: close\r\n\r\n", size);
            close(fd);
            return;
        }
    }
    long long len = size ? end - start + 1 : 0;
    if (partial)
        sendf(s, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %lld-%lld/%lld\r\n", start, end, size);
    else
        sendf(s, "HTTP/1.1 200 OK\r\n");
    sendf(s, "Content-Type: %s\r\nContent-Length: %lld\r\nAccept-Ranges: bytes\r\n"
             "Content-Disposition: inline\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
          mime, len);
    lseek(fd, start, SEEK_SET);
    char *buf = malloc(IO_BUF);
    while (buf && len > 0) {
        ssize_t r = read(fd, buf, len < IO_BUF ? (size_t)len : IO_BUF);
        if (r <= 0 || send_all(s, buf, r)) break;
        len -= r;
    }
    free(buf);
    close(fd);
}

/* Installed games and apps, with icons. Scans on its own (the TV's Apps tab has its own copy). */
static void send_apps(int s)
{
    sendf(s, "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
             "Connection: close\r\n\r\n");
    send_all(s, PAGE_HEAD, strlen(PAGE_HEAD));
    send_nav(s, "apps");
    sendf(s, "</header><main><div class=tools><b id=sum></b><span class=sp></span>"
             "<input type=search id=flt placeholder='Filter by name or ID'></div>"
             "<table id=apps><thead><tr class=dim style='font-size:13px'>"
             "<th style='text-align:left;cursor:pointer' data-c=0>Name &#8597;</th>"
             "<th class='s dt' style='cursor:pointer' data-c=1>ID &#8597;</th>"
             "<th class=s style='cursor:pointer' data-c=2>Version &#8597;</th>"
             "<th class=s style='cursor:pointer' data-c=3>Size &#8597;</th></tr></thead><tbody>");
    DIR *d = opendir("/user/app");
    int n = 0;
    unsigned long long total = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) != 9) continue;
        char sfo[256], title[256], ver[32], cat[16], esc[600], p[256], a1[32], a2[48] = "";
        snprintf(sfo, sizeof sfo, "/system_data/priv/appmeta/%s/param.sfo", e->d_name);
        if (sfo_get(sfo, "TITLE", title, sizeof title)) snprintf(title, sizeof title, "%s", e->d_name);
        sfo_get(sfo, "APP_VER", ver, sizeof ver);
        sfo_get(sfo, "CATEGORY", cat, sizeof cat);
        html_escape(title, esc, sizeof esc);
        struct kstat st;
        unsigned long long sz = 0;
        snprintf(p, sizeof p, "/user/app/%s/app.pkg", e->d_name);
        if (k_stat(p, &st) == 0) sz = st.st_size;
        snprintf(p, sizeof p, "/user/patch/%s/patch.pkg", e->d_name);
        if (k_stat(p, &st) == 0) {
            char t[32];
            fmt_size(t, sizeof t, st.st_size);
            snprintf(a2, sizeof a2, "<br><span class=dim>+ update %s</span>", t);
            sz += st.st_size;
        }
        total += sz;
        fmt_size(a1, sizeof a1, sz);
        /* everything from param.sfo / folder names is escaped: a crafted package must not be
           able to inject script into this page */
        char idesc[64], idenc[96], vesc[128], cesc[64];
        html_escape(e->d_name, idesc, sizeof idesc);
        url_encode(e->d_name, idenc, sizeof idenc);
        html_escape(ver[0] ? ver : "?", vesc, sizeof vesc);
        html_escape(apps_category_name(cat), cesc, sizeof cesc);
        /* data-s = raw bytes / text, so the table can sort by real values */
        sendf(s, "<tr><td data-s=\"%s\"><img class=ic loading=lazy src='/view?p=/user/appmeta/%s/icon0.png' alt=''>"
                 "<a href='/?p=/user/app/%s'>%s</a></td><td class='s dt' data-s=\"%s\">%s</td>"
                 "<td class=s data-s=\"%s\">%s v%s</td><td class=s data-s=%llu>%s%s</td></tr>",
              esc, idenc, idenc, esc, idesc, idesc, vesc, cesc, vesc, sz, a1, a2);
        n++;
    }
    if (d) closedir(d);
    if (!n) sendf(s, "<tr><td>No installed apps found (root access needed).</td></tr>");
    char tb[32];
    fmt_size(tb, sizeof tb, total);
    sendf(s, "</tbody></table></main><script>"
             "document.getElementById('sum').textContent='%d installed - %s (games + updates)';", n, tb);
    static const char *JS =
        "const tb=document.querySelector('#apps tbody');let col=0,dir=1;"
        "document.querySelectorAll('th[data-c]').forEach(th=>th.onclick=()=>{const c=+th.dataset.c;"
        "dir=c===col?-dir:(c===3?-1:1);col=c;[...tb.rows].sort((a,b)=>{const x=a.cells[c].dataset.s,y=b.cells[c].dataset.s;"
        "return(c===3?(x-y):x.localeCompare(y,undefined,{numeric:true,sensitivity:'base'}))*dir})"
        ".forEach(r=>tb.appendChild(r))});"
        "document.getElementById('flt').oninput=e=>{const q=e.target.value.toLowerCase();"
        "[...tb.rows].forEach(r=>r.style.display=r.textContent.toLowerCase().includes(q)?'':'none')};"
        "</script></body></html>";
    send_all(s, JS, strlen(JS));
}

/* ---------- text editor ---------- */
#define EDIT_MAX (2 * 1024 * 1024)

/* Sends text HTML-escaped, in chunks (files can be large). */
static void send_escaped(int s, const char *p, size_t n)
{
    char out[8192];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char *r = NULL;
        switch (p[i]) {
        case '&': r = "&amp;"; break;
        case '<': r = "&lt;"; break;
        case '>': r = "&gt;"; break;
        }
        if (k > sizeof out - 8) { send_all(s, out, k); k = 0; }
        if (r) { size_t l = strlen(r); memcpy(out + k, r, l); k += l; }
        else out[k++] = p[i];
    }
    if (k) send_all(s, out, k);
}

static void send_editor(int s, const char *path)
{
    struct kstat st;
    int fd = open_regular(path, &st);
    if (fd == -2) { reply_text(s, 403, "Forbidden", "Not a regular file."); return; }
    if (fd < 0) { reply_text(s, 404, "Not Found", "File not found."); return; }
    if (st.st_size > EDIT_MAX) { close(fd); reply_text(s, 413, "Too Large", "Files over 2 MB can't be edited here."); return; }
    char *buf = malloc(st.st_size + 1);
    ssize_t got = 0;
    while (buf && fd >= 0 && got < st.st_size) {
        ssize_t r = read(fd, buf + got, st.st_size - got);
        if (r <= 0) break;
        got += r;
    }
    if (fd >= 0) close(fd);
    if (!buf || got != st.st_size) { free(buf); reply_text(s, 500, "Error", "Couldn't read the file."); return; }
    if (memchr(buf, 0, got)) { free(buf); reply_text(s, 415, "Binary", "This is a binary file and can't be edited as text."); return; }

    char enc[3072], esc[1024], dir[1024];
    url_encode(path, enc, sizeof enc);
    html_escape(path, esc, sizeof esc);
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl == dir) dir[1] = 0; else if (sl) *sl = 0;
    char denc[3072];
    url_encode(dir, denc, sizeof denc);

    sendf(s, "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
             "Connection: close\r\n\r\n");
    send_all(s, PAGE_HEAD, strlen(PAGE_HEAD));
    send_nav(s, "files");
    sendf(s, "</header><main><p class=crumb>%s</p>"
             "<p><button onclick=save()>Save</button> <a href='/?p=%s'>Back to folder</a> "
             "<span id=msg style='color:#8995a1'></span></p>"
             "<textarea id=t spellcheck=false style='width:100%%;height:70vh;box-sizing:border-box;"
             "background:#1a2129;color:#e8ecef;border:1px solid #2a333d;font:14px monospace;padding:10px'>\n",
          esc, denc);   /* that "\n" is eaten by the browser, so a leading blank line in the file survives */
    int crlf = 0;
    for (ssize_t i = 0; i + 1 < got; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n') { crlf = 1; break; }
    send_escaped(s, buf, got);
    free(buf);
    /* browsers turn CRLF into LF in a textarea: convert back on save if the file used CRLF */
    sendf(s, "</textarea><script>const P='%s',CRLF=%d,t=document.getElementById('t'),msg=document.getElementById('msg');"
             "let dirty=false;t.oninput=()=>{dirty=true;msg.textContent='unsaved changes'};"
             "function save(){msg.textContent='Saving...';const v=CRLF?t.value.replace(/\\r?\\n/g,'\\r\\n'):t.value;"
             "fetch('/save?p='+P,{method:'POST',headers:{'X-TBox':'1'},body:v})"
             ".then(r=>r.text().then(x=>{if(r.ok)dirty=false;msg.textContent=x}),()=>msg.textContent='Connection lost')}"
             "document.onkeydown=e=>{if((e.ctrlKey||e.metaKey)&&e.key=='s'){e.preventDefault();save()}};"
             "window.onbeforeunload=e=>{if(dirty){e.preventDefault();e.returnValue=''}};"
             "</script></main></body></html>", enc, crlf);
}

static void receive_save(int s, const char *path, long long clen, const char *pre, int prelen)
{
    struct kstat st;
    if (k_stat(path, &st) != 0) { reply_text(s, 404, "Not Found", "File not found."); return; }
    if (!S_ISREG(st.st_mode)) { reply_text(s, 403, "Forbidden", "Not a regular file."); return; }
    if (clen < 0 || clen > EDIT_MAX) { reply_text(s, 413, "Too Large", "Text too large (2 MB max)."); return; }
    char tmp[1100], bak[1100];
    snprintf(tmp, sizeof tmp, "%s.tbox_tmp", path);
    snprintf(bak, sizeof bak, "%s.bak", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (fd < 0) { reply_text(s, 500, "Error", "Can't write in this folder."); return; }
    long long got = 0;
    int ok = 1;
    if (prelen > 0) {
        int take = prelen > clen ? (int)clen : prelen;
        if (write(fd, pre, take) != take) ok = 0;
        got = take;
    }
    char *buf = malloc(IO_BUF);
    while (ok && buf && got < clen) {
        int r = sceNetRecv(s, buf, clen - got > IO_BUF ? IO_BUF : (size_t)(clen - got), 0);
        if (r <= 0) { ok = 0; break; }
        if (write(fd, buf, r) != r) ok = 0;
        got += r;
    }
    free(buf);
    close(fd);
    if (!ok || got != clen) { unlink(tmp); reply_text(s, 500, "Error", "Save failed (connection lost or disk full)."); return; }
    unlink(bak);
    rename(path, bak);                      /* previous version kept as .bak */
    if (rename(tmp, path) != 0) { rename(bak, path); unlink(tmp); reply_text(s, 500, "Error", "Save failed."); return; }
    reply_text(s, 200, "OK", "Saved (previous version kept as .bak)");
    wlog("Edited %s", path_base(path));
}

/* ---------- capture gallery ---------- */
static int is_capture(const char *name, int *video)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    *video = !strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".webm");
    return *video || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png");
}

/* Lists captures in dir (and its sub-folders, one per game), newest folders as found. */
static int gallery_dir(int s, const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int shown = 0, header = 0;
    struct dirent *e;
    char full[1024], enc[3072], esc[600];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (path_join(full, sizeof full, dir, e->d_name)) continue;
        struct kstat st;
        if (k_stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 3) shown += gallery_dir(s, full, depth + 1);
            continue;
        }
        int video;
        if (!is_capture(e->d_name, &video)) continue;
        if (!header) {
            html_escape(path_base(dir), esc, sizeof esc);
            sendf(s, "<h3>%s</h3><div class=g>", esc);
            header = 1;
        }
        url_encode(full, enc, sizeof enc);
        html_escape(e->d_name, esc, sizeof esc);
        if (video)
            sendf(s, "<figure><video preload=metadata controls src='/view?p=%s'></video>"
                     "<figcaption>%s <a href='/dl?p=%s'>&#11015;</a></figcaption></figure>", enc, esc, enc);
        else
            sendf(s, "<figure><a href='/view?p=%s' target=_blank><img loading=lazy src='/view?p=%s' alt=''></a>"
                     "<figcaption>%s <a href='/dl?p=%s'>&#11015;</a></figcaption></figure>", enc, enc, esc, enc);
        shown++;
    }
    closedir(d);
    if (header) sendf(s, "</div>");
    return shown;
}

static void send_gallery(int s)
{
    sendf(s, "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
             "Connection: close\r\n\r\n");
    send_all(s, PAGE_HEAD, strlen(PAGE_HEAD));
    send_nav(s, "gallery");
    sendf(s, "</header><main><style>.g{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:12px}"
             "figure{margin:0;background:#1a2129;border-radius:6px;overflow:hidden}"
             "figure img,figure video{width:100%%;aspect-ratio:16/9;object-fit:cover;display:block;background:#000}"
             "figcaption{padding:6px 8px;font-size:13px;color:#8995a1;word-break:break-all}</style>"
             "<h2>Screenshots</h2>");
    if (!gallery_dir(s, "/user/av_contents/photo", 0))
        sendf(s, "<p style='color:#8995a1'>No screenshots yet - press SHARE on the controller in a game.</p>");
    sendf(s, "<h2>Videos</h2>");
    if (!gallery_dir(s, "/user/av_contents/video", 0))
        sendf(s, "<p style='color:#8995a1'>No video clips yet.</p>");
    sendf(s, "</main></body></html>");
}

/* ---------- dashboard ---------- */
static void json_str(char *out, size_t n, const char *in)
{
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && k + 7 < n; p++) {
        if (*p == '"' || *p == '\\') { out[k++] = '\\'; out[k++] = *p; }
        else if (*p < 32) k += snprintf(out + k, n - k, "\\u%04x", *p);
        else out[k++] = *p;
    }
    out[k] = 0;
}

/* GET /api/status - live numbers for the dashboard (JSON) */
static void send_status(int s)
{
    char fw[96], ipb[32], fwj[200], buf[8192];
    sysinfo_firmware(fw, sizeof fw);
    sysinfo_ip(ipb, sizeof ipb);
    json_str(fwj, sizeof fwj, fw);
    int k = snprintf(buf, sizeof buf,
        "{\"fw\":\"%s\",\"ip\":\"%s\",\"cpu\":%d,\"soc\":%d,\"up\":%llu,\"web\":%d,"
        "\"root\":%d,\"disks\":[",
        fwj, ipb, sysinfo_cpu_temp(), sysinfo_soc_temp(),
        (unsigned long long)(sceKernelGetProcessTime() / 1000000ULL),
        web_running(), is_dir("/system"));
    static const char *dn[] = { "Internal", "USB 0", "USB 1" }, *dp[] = { "/user", "/mnt/usb0", "/mnt/usb1" };
    int first = 1;
    for (int i = 0; i < 3; i++) {
        unsigned long long t, f;
        if ((i && !is_dir(dp[i])) || disk_space(dp[i], &t, &f) != 0) continue;
        k += snprintf(buf + k, sizeof buf - k, "%s{\"n\":\"%s\",\"p\":\"%s\",\"t\":%llu,\"f\":%llu}",
                      first ? "" : ",", dn[i], dp[i], t, f);
        first = 0;
    }
    /* installed apps (count, games + updates size) and payload count: these walk folders and
       USB drives, so they are cached for a minute instead of redone on every 5 s refresh */
    static pthread_mutex_t scan_mtx = PTHREAD_MUTEX_INITIALIZER;
    static unsigned long long scan_at, used_c;
    static int napp_c, npl_c, scanned;
    unsigned long long now_us = sceKernelGetProcessTime();
    pthread_mutex_lock(&scan_mtx);
    if (!scanned || now_us - scan_at > 60ULL * 1000000) {
        int napp = 0;
        unsigned long long used = 0;
        DIR *d = opendir("/user/app");
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.' || strlen(e->d_name) != 9) continue;
            char p[128];
            struct kstat st;
            napp++;
            snprintf(p, sizeof p, "/user/app/%s/app.pkg", e->d_name);
            if (k_stat(p, &st) == 0) used += st.st_size;
            snprintf(p, sizeof p, "/user/patch/%s/patch.pkg", e->d_name);
            if (k_stat(p, &st) == 0) used += st.st_size;
        }
        if (d) closedir(d);
        char (*pl)[512] = malloc(PAYLOAD_MAX * 512);
        int npl = pl ? payload_scan(pl, PAYLOAD_MAX) : 0;
        free(pl);
        napp_c = napp; used_c = used; npl_c = npl; scan_at = now_us; scanned = 1;
    }
    int napp = napp_c, npl = npl_c;
    unsigned long long used = used_c;
    pthread_mutex_unlock(&scan_mtx);
    char autop[600];
    json_str(autop, sizeof autop, cfg.autopayload[0] ? path_base(cfg.autopayload) : "");
    k += snprintf(buf + k, sizeof buf - k, "],\"apps\":%d,\"appsize\":%llu,\"payloads\":%d,\"autopayload\":\"%s\"",
                  napp, used, npl, autop);

    /* temperature history (oldest first, one sample per TEMP_STEP_S seconds) and peaks */
    int hc[TEMP_HIST], hs[TEMP_HIST], pc, ps;
    int nh = sysinfo_temp_history(hc, hs, TEMP_HIST, &pc, &ps);
    k += snprintf(buf + k, sizeof buf - k, ",\"step\":%d,\"peak\":[%d,%d],\"hc\":[", TEMP_STEP_S, pc, ps);
    for (int i = 0; i < nh; i++) k += snprintf(buf + k, sizeof buf - k, "%s%d", i ? "," : "", hc[i]);
    k += snprintf(buf + k, sizeof buf - k, "],\"hs\":[");
    for (int i = 0; i < nh; i++) k += snprintf(buf + k, sizeof buf - k, "%s%d", i ? "," : "", hs[i]);

    /* recent activity: newest first, from the web log shown on the PS4's Network tab */
    k += snprintf(buf + k, sizeof buf - k, "],\"reqs\":%d,\"wl\":[", nrequests);
    char line[160], lj[400];
    for (int i = 0; i < LOG_N && web_log_line(i, line, sizeof line); i++) {
        json_str(lj, sizeof lj, line);
        k += snprintf(buf + k, sizeof buf - k, "%s\"%s\"", i ? "," : "", lj);
    }
    snprintf(buf + k, sizeof buf - k, "]}");
    sendf(s, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
             "Content-Length: %d\r\nConnection: close\r\n\r\n", (int)strlen(buf));
    send_all(s, buf, strlen(buf));
}

static void send_dashboard(int s)
{
    sendf(s, "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
             "Connection: close\r\n\r\n");
    send_all(s, PAGE_HEAD, strlen(PAGE_HEAD));
    send_nav(s, "home");
    static const char *BODY =
        "</header><main><style>.wide{grid-column:span 2}@media(max-width:700px){.wide{grid-column:auto}}"
        ".sw{display:inline-block;width:14px;height:4px;border-radius:2px;vertical-align:middle;margin-right:6px}"
        ".act{font-size:13px;padding:3px 0;border-bottom:1px solid #222a33;overflow-wrap:anywhere}"
        ".act:last-child{border:0}.sb{padding:3px 10px;margin-left:8px;font-size:13px}</style>"
        "<div class=cards id=cards><div class=card><h3>Loading...</h3></div></div>"
        "<div class=tiles>"
        "<a class=tile href='/?p=/'><b>&#128193; Files</b><span>Browse the whole console</span></a>"
        "<a class=tile href='/?p=/user'><b>&#128190; Internal storage</b><span>/user</span></a>"
        "<a class=tile id=usbtile href='/?p=/mnt/usb0'><b>&#128268; USB drive</b><span id=usbsub>/mnt/usb0</span></a>"
        "<a class=tile href='/apps'><b>&#127918; Apps</b><span>Installed games with icons</span></a>"
        "<a class=tile href='/gallery'><b>&#128247; Gallery</b><span>Screenshots and videos</span></a>"
        "<a class=tile href='/payload'><b>&#128640; Send payload</b><span>To GoldHEN BinLoader</span></a>"
        "<a class=tile href='/search?p=/user'><b>&#128269; Search</b><span>Find files on internal storage</span></a>"
        "<a class=tile href='/log'><b>&#128221; Debug log</b><span>What the app has been doing</span></a>"
        "</div><p class=dim id=upd style='font-size:13px;margin-top:16px'></p></main>"
        "<script>"
        "const fmt=b=>{const u=['B','KB','MB','GB','TB'];let i=0;while(b>=1024&&i<4){b/=1024;i++}return(i?b.toFixed(1):b)+' '+u[i]};"
        "const esc=t=>String(t).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));"
        "const kv=(k,v,c)=>`<div class=kv><span>${k}</span><span class='${c||''}'>${v}</span></div>`;"
        "const tc=t=>t<0?'dim':t>=75?'bad':t>=65?'warn':'ok';"
        /* temperature graph: 60 samples (10 min), 30-90 C, guide lines at 50/65/75 */
        "function spark(a,b){const W=600,H=130,N=60,lo=30,hi=90;"
        "const y=t=>(H-(Math.min(hi,Math.max(lo,t))-lo)*H/(hi-lo)).toFixed(1);"
        "const pts=v=>v.map((t,i)=>t<0?'':(W-(v.length-1-i)*W/(N-1)).toFixed(1)+','+y(t)).filter(Boolean).join(' ');"
        "const g=[50,65,75].map(t=>`<line x1=0 x2=${W} y1=${y(t)} y2=${y(t)} stroke='var(--${t==75?'bad':t==65?'warn':'line'})' "
        "stroke-dasharray='4 4'/><text x=4 y=${y(t)-4} fill='var(--dim)' font-size=12>${t}&deg;</text>`).join('');"
        "return `<svg viewBox='0 0 ${W} ${H}' style='width:100%;height:auto;display:block;margin:8px 0'>${g}"
        "<polyline points='${pts(a)}' fill='none' stroke='var(--acc)' stroke-width='2.5' stroke-linejoin='round'/>"
        "<polyline points='${pts(b)}' fill='none' stroke='#b07cff' stroke-width='2.5' stroke-linejoin='round'/></svg>`}"
        "const tv=(t,p)=>t<0?'n/a':t+' &deg;C'+(p>t?` <span class=dim>(peak ${p})</span>`:'');"
        "const acts=(l,e)=>l.length?l.map(x=>`<div class=act>${esc(x)}</div>`).join(''):`<div class='act dim'>${e}</div>`;"
        "async function load(){if(document.hidden)return;try{const r=await fetch('/api/status');const d=await r.json();"
        "let st='';for(const k of d.disks){const used=k.t?1-k.f/k.t:0;"
        "st+=kv(k.n,fmt(k.f)+' free of '+fmt(k.t))+`<div class=meter><div style='width:${(used*100).toFixed(1)}%;"
        "background:var(--${used>.9?'bad':used>.75?'warn':'acc'})'></div></div>`}"
        "const up=d.up,h=Math.floor(up/3600),m=Math.floor(up/60)%60;"
        "document.getElementById('cards').innerHTML="
        "`<div class='card wide'><h3>Temperature</h3>"
        "${kv(`<span class=sw style='background:var(--acc)'></span>CPU`,tv(d.cpu,d.peak[0]),tc(d.cpu))}"
        "${kv(`<span class=sw style='background:#b07cff'></span>SoC`,tv(d.soc,d.peak[1]),tc(d.soc))}"
        "${d.hc.length>1?spark(d.hc,d.hs):`<p class=dim>Collecting readings - the graph fills in over the next minutes.</p>`}"
        "<span class=dim style='font-size:12px'>Last 10 minutes, one reading every ${d.step} s. Above 75 &deg;C is hot.</span></div>`+"
        "`<div class=card><h3>System</h3>${kv('Firmware',esc(d.fw.split(' ')[0]))}"
        "${kv('Access',d.root?'ROOT':'Sandboxed',d.root?'ok':'warn')}"
        "${kv('GoldHEN',d.root?'active':'not detected',d.root?'ok':'dim')}"
        "${kv('App uptime',h+'h '+m+'m')}</div>`+"
        "`<div class=card><h3>Network</h3>${kv('IP',esc(d.ip))}"
        "${kv('FTP','GoldHEN, port 2121','dim')}"
        "${kv('Web requests',d.reqs)}</div>`+"
        "`<div class=card><h3>Storage</h3>${st||'<span class=dim>n/a</span>'}</div>`+"
        "`<div class=card><h3>Apps &amp; payloads</h3>${kv('Installed',d.apps)}${kv('Using',fmt(d.appsize))}"
        "${kv('Payloads found',d.payloads)}${kv('Auto payload',d.autopayload?esc(d.autopayload):'none',d.autopayload?'ok':'dim')}</div>`+"
        "`<div class='card wide'><h3>Recent activity</h3>${acts(d.wl,'Nothing yet')}</div>`;"
        "const usb=d.disks.find(x=>x.p.startsWith('/mnt/usb'));const ut=document.getElementById('usbtile');"
        "if(usb){ut.href='/?p='+usb.p;document.getElementById('usbsub').textContent=usb.p+' - '+fmt(usb.f)+' free'}"
        "else{ut.removeAttribute('href');ut.style.opacity=.5;document.getElementById('usbsub').textContent='Not plugged in'}"
        "document.getElementById('upd').textContent='Updated '+new Date().toLocaleTimeString()+' - refreshes every 5 s'"
        "}catch(e){document.getElementById('upd').textContent='Lost connection to the PS4 - retrying...'}}"
        "load();setInterval(load,5000);"
        "document.addEventListener('visibilitychange',()=>{if(!document.hidden)load()});"   /* no polling in background tabs */
        "</script></body></html>";
    send_all(s, BODY, strlen(BODY));
}

/* ---------- settings (shared with the TV's Settings tab) ---------- */
static void send_settings_json(int s)
{
    char buf[512];
    snprintf(buf, sizeof buf,
             "{\"web_auto\":%d,\"start_last\":%d,\"theme\":%d}",
             cfg.web_autostart, cfg.start_last, cfg.theme);
    sendf(s, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
             "Content-Length: %d\r\nConnection: close\r\n\r\n%s", (int)strlen(buf), buf);
}

/* POST /api/set?k=KEY&v=VALUE */
static void receive_setting(int s, const char *k, const char *v)
{
    int n = atoi(v), b = (n != 0);
    char msg[160] = "Saved";
    if (!strcmp(k, "web_auto")) cfg.web_autostart = b;
    else if (!strcmp(k, "start_last")) cfg.start_last = b;
    else if (!strcmp(k, "theme") && n >= 0 && n < 4) cfg.theme = n;
    else { reply_text(s, 400, "Bad Request", "Unknown setting."); return; }
    if (config_save() != 0) { reply_text(s, 500, "Error", "Couldn't save settings."); return; }
    reply_text(s, 200, "OK", msg);
}

static void send_settings_page(int s)
{
    sendf(s, "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
             "Connection: close\r\n\r\n");
    send_all(s, PAGE_HEAD, strlen(PAGE_HEAD));
    send_nav(s, "settings");
    static const char *BODY =
        "</header><main><div class=cards>"
        "<div class=card><h3>Servers</h3>"
        "<label class=kv><span>Start web page when the app opens</span><input type=checkbox data-k=web_auto></label>"
        "<p class=dim style='font-size:13px'>The web page itself is running - stop it from the PS4. "
        "For FTP use GoldHEN's server (port 2121).</p></div>"
        "<div class=card><h3>App</h3>"
        "<div class=kv><span>Theme (TV)</span><select class=t id=theme>"
        "<option value=0>Ocean</option><option value=1>Violet</option><option value=2>Forest</option>"
        "<option value=3>Ember</option></select></div>"
        "<div class=kv><span>When the app opens</span><select class=t id=start>"
        "<option value=0>Home dashboard</option><option value=1>Last tab and folder</option></select></div></div>"
        "</div><p id=msg></p></main><script>"
        "const msg=document.getElementById('msg');"
        "async function set(k,v){try{const r=await fetch('/api/set?k='+k+'&v='+encodeURIComponent(v),{method:'POST',headers:{'X-TBox':'1'}});"
        "msg.className=r.ok?'ok':'bad';msg.textContent=await r.text()}catch(e){msg.className='bad';"
        "msg.textContent='Lost connection'}load()}"
        "async function load(){const r=await fetch('/api/settings');const d=await r.json();"
        "document.querySelectorAll('[data-k]').forEach(c=>c.checked=!!d[c.dataset.k]);"
        "document.getElementById('theme').value=d.theme;document.getElementById('start').value=d.start_last}"
        "document.querySelectorAll('[data-k]').forEach(c=>c.onchange=()=>set(c.dataset.k,c.checked?1:0));"
        "document.getElementById('theme').onchange=e=>set('theme',e.target.value);"
        "document.getElementById('start').onchange=e=>set('start_last',e.target.value);"
        "load();</script></body></html>";
    send_all(s, BODY, strlen(BODY));
}

static void send_logpage(int s)
{
    sendf(s, "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
             "Connection: close\r\n\r\n");
    send_all(s, PAGE_HEAD, strlen(PAGE_HEAD));
    send_nav(s, "log");
    sendf(s, "</header><main><div class=tools><b>Debug log</b><span class=sp></span>"
             "<a href='/log'>Refresh</a> <a href='/dl?p=" DLOG_FILE "'>Download</a> "
             "<a href='/view?p=/data/OrbisKit/log_prev.txt'>Previous run</a></div><pre>");
    int fd = open(DLOG_FILE, O_RDONLY, 0);
    if (fd >= 0) {
        char buf[8192];
        ssize_t r;
        while ((r = read(fd, buf, sizeof buf)) > 0) send_escaped(s, buf, r);
        close(fd);
    } else {
        sendf(s, "(no log yet)");
    }
    sendf(s, "</pre></main><script>scrollTo(0,document.body.scrollHeight)</script></body></html>");
}

static void send_payload_page(int s)
{
    sendf(s, "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
             "Connection: close\r\n\r\n");
    send_all(s, PAGE_HEAD, strlen(PAGE_HEAD));
    send_nav(s, "payload");
    static const char *BODY =
        "</header><main><h2>Send payload</h2>"
        "<p>Pick a <b>.bin</b> payload on this device. It is sent to GoldHEN's BinLoader on the PS4 "
        "(port 9090 - enable BinLoader in GoldHEN settings).</p>"
        "<p><label class=b>Choose .bin and send<input id=pf type=file accept='.bin' hidden></label></p>"
        "<div id=msg></div>"
        "<script>const pf=document.getElementById('pf'),msg=document.getElementById('msg');"
        "pf.onchange=()=>{const f=pf.files[0];if(!f)return;msg.textContent='Sending '+f.name+'...';"
        "fetch('/payload?n='+encodeURIComponent(f.name),{method:'POST',headers:{'X-TBox':'1'},body:f})"
        ".then(r=>r.text().then(t=>{msg.textContent=(r.ok?'OK: ':'Failed: ')+t}),()=>{msg.textContent='Connection lost'});"
        "pf.value=''}</script></main></body></html>";
    send_all(s, BODY, strlen(BODY));
}

static void receive_payload(int s, const char *name, long long clen, const char *pre, int prelen)
{
    if (clen <= 0 || clen > 64LL * 1024 * 1024) { reply_text(s, 400, "Bad Request", "Payload must be 1 byte to 64 MB."); return; }
    char *buf = malloc(clen);
    if (!buf) { reply_text(s, 500, "Error", "Out of memory."); return; }
    long long got = prelen > clen ? clen : prelen;
    if (got > 0) memcpy(buf, pre, got);
    while (got < clen) {
        int r = sceNetRecv(s, buf + got, clen - got > IO_BUF ? IO_BUF : (size_t)(clen - got), 0);
        if (r <= 0) break;
        got += r;
    }
    char err[160];
    if (got != clen) reply_text(s, 400, "Bad Request", "Upload interrupted.");
    else if (payload_send_buf(buf, (long)clen, err, sizeof err) == 0) {
        reply_text(s, 200, "OK", "Payload sent to the PS4.");
        wlog("Payload sent from browser: %s", name[0] ? name : "(unnamed)");
        dlog("web: payload %s (%lld bytes) sent", name, clen);
    } else {
        reply_text(s, 502, "Error", err);
    }
    free(buf);
}

static void send_file(int s, const char *path)
{
    struct kstat st;
    int fd = open_regular(path, &st);
    if (fd == -2) { reply_text(s, 403, "Forbidden", "Not a regular file."); return; }
    if (fd < 0) { reply_text(s, 404, "Not Found", "File not found."); return; }
    char name[256];
    snprintf(name, sizeof name, "%s", path_base(path));
    for (char *c = name; *c; c++) if (*c == '"' || *c < 32) *c = '_';
    sendf(s, "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %llu\r\n"
             "Content-Disposition: attachment; filename=\"%s\"\r\nConnection: close\r\n\r\n",
          (unsigned long long)st.st_size, name);
    char *buf = malloc(IO_BUF);
    ssize_t r;
    while (buf && (r = read(fd, buf, IO_BUF)) > 0)
        if (send_all(s, buf, r)) break;
    free(buf);
    close(fd);
    wlog("Downloaded %s", path_base(path));
}

/* Relative sub-folder from a folder upload ("photos/2024"): every part must be a plain name. */
static int valid_subpath(const char *sub)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", sub);
    char *save = NULL;
    int parts = 0;
    for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!valid_name(t)) return 0;
        parts++;
    }
    return parts > 0;
}

static void receive_file(int s, const char *dir, const char *name, const char *sub, long long clen,
                         const char *pre, int prelen)
{
    if (!valid_name(name) || !is_dir(dir)) { reply_text(s, 400, "Bad Request", "Bad folder or file name."); return; }
    if (clen < 0) { reply_text(s, 411, "Length Required", "Content-Length required."); return; }
    char target[1024], path[1024];
    if (sub[0]) {
        if (!valid_subpath(sub) || path_join(target, sizeof target, dir, sub)) {
            reply_text(s, 400, "Bad Request", "Bad sub-folder name.");
            return;
        }
        if (mkdirs(target) != 0) { reply_text(s, 500, "Error", "Can't create the folder."); return; }
    } else {
        snprintf(target, sizeof target, "%s", dir);
    }
    /* a truncated path is a different file: never write (and rename over) it */
    if (path_join(path, sizeof path, target, name)) { reply_text(s, 400, "Bad Request", "Path too long."); return; }
    /* write to a temporary file and only replace the real one when everything arrived:
       a dropped connection must not destroy an existing file of the same name */
    char part[1100];
    snprintf(part, sizeof part, "%s.tbox_part", path);
    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (fd < 0) { reply_text(s, 500, "Error", strerror(errno)); return; }

    long long got = 0;
    int ok = 1;
    if (prelen > 0) {
        int take = prelen > clen ? (int)clen : prelen;
        if (write(fd, pre, take) != take) ok = 0;
        got = take;
    }
    char *buf = malloc(IO_BUF);
    if (!buf) ok = 0;
    while (ok && got < clen) {
        long long want = clen - got;
        int r = sceNetRecv(s, buf, want < IO_BUF ? (size_t)want : IO_BUF, 0);
        if (r <= 0) { ok = 0; break; }
        int off = 0;
        while (off < r) {
            ssize_t w = write(fd, buf + off, r - off);
            if (w <= 0) { ok = 0; break; }
            off += w;
        }
        got += r;
    }
    free(buf);
    if (close(fd) != 0) ok = 0;
    if (ok && got == clen && rename(part, path) == 0) {
        reply_text(s, 200, "OK", "Uploaded");
        wlog("Uploaded %s", name);
    } else {
        unlink(part);
        reply_text(s, 500, "Error", "Upload failed (connection lost or disk full). Any existing file was kept.");
    }
}

/* ---------- connection ---------- */
static void *conn_serve(void *arg)
{
    int s = (int)(intptr_t)arg;
    /* the PS4 kernel is FreeBSD: SO_RCVTIMEO/SO_SNDTIMEO take a struct timeval; an int
       (the Vita-style microseconds form) is rejected, so only fall back to it */
    struct { long sec, usec; } tv = { RECV_TIMEOUT_US / 1000000, 0 };
    int tmo = RECV_TIMEOUT_US;
    int r1 = sceNetSetsockopt(s, NET_SOL_SOCKET, NET_SO_RCVTIMEO, &tv, sizeof tv);
    if (r1 < 0) r1 = sceNetSetsockopt(s, NET_SOL_SOCKET, NET_SO_RCVTIMEO, &tmo, sizeof tmo);
    int r2 = sceNetSetsockopt(s, NET_SOL_SOCKET, NET_SO_SNDTIMEO, &tv, sizeof tv);   /* client stopped reading */
    if (r2 < 0) r2 = sceNetSetsockopt(s, NET_SOL_SOCKET, NET_SO_SNDTIMEO, &tmo, sizeof tmo);
    static int logged;
    if (!logged) { logged = 1; dlog("web: socket timeouts rcv=%x snd=%x", r1, r2); }
    char hdr[HDR_MAX + 1];
    int len = 0, hend = -1;
    while (len < HDR_MAX) {
        int r = sceNetRecv(s, hdr + len, HDR_MAX - len, 0);
        if (r <= 0) break;
        len += r;
        hdr[len] = 0;
        char *e = strstr(hdr, "\r\n\r\n");
        if (e) { hend = (int)(e - hdr) + 4; break; }
    }
    if (hend < 0) { sceNetSocketClose(s); return NULL; }
    nrequests++;

    char method[8] = "", target[2048] = "";
    sscanf(hdr, "%7s %2047s", method, target);
    long long clen = -1;
    /* header loops stop before the blank line: l + 2 must never reach the body (hdr + hend),
       or a body starting with "X-TBox:" / "Content-Length:" would count as a header */
    for (char *l = strstr(hdr, "\r\n"); l && l < hdr + hend - 2; l = strstr(l + 2, "\r\n"))
        if (!strncasecmp(l + 2, "Content-Length:", 15)) {
            const char *v = l + 17;
            while (*v == ' ' || *v == '\t') v++;   /* "Content-Length:123" has no space */
            clen = atoll(v);
        }

    char *query = strchr(target, '?');
    if (query) *query++ = 0; else query = "";
    char p[1024], n[512];
    get_param(query, "p", p, sizeof p);
    get_param(query, "n", n, sizeof n);
    if (!p[0]) strcpy(p, "/");
    /* a path that filled the buffer was cut short - and a cut path is a parent folder */
    if (strlen(p) >= sizeof p - 1 || strlen(n) >= sizeof n - 1 || norm_path(p, sizeof p) != 0) {
        reply_text(s, 400, "Bad Request", "Path too long or too deep.");
        sceNetSocketClose(s);
        return NULL;
    }

    /* DNS-rebinding guard: a website can point its own domain at the PS4's IP, and the
       browser then treats our pages as that site's. Real visitors type the IP, so only
       accept a Host header that is an IPv4 address (or localhost), optionally with :port. */
    for (char *l = strstr(hdr, "\r\n"); l && l < hdr + hend - 2; l = strstr(l + 2, "\r\n")) {
        if (strncasecmp(l + 2, "Host:", 5)) continue;
        const char *h = l + 7;
        while (*h == ' ') h++;
        int ok = !strncasecmp(h, "localhost", 9) && (h[9] == ':' || h[9] == '\r');
        if (!ok) {
            ok = 1;
            const char *c = h;
            for (; *c && *c != '\r' && *c != ':'; c++) if (!((*c >= '0' && *c <= '9') || *c == '.')) ok = 0;
            if (c == h) ok = 0;
        }
        if (!ok) {
            reply_text(s, 403, "Forbidden", "Open this page by the PS4's IP address: http://ps4-ip:8080");
            sceNetSocketClose(s);
            return NULL;
        }
        break;
    }

    /* CSRF guard: every state-changing request must carry the custom header X-TBox that our
       own pages add. Other websites open in a browser on the LAN can't add custom headers to
       requests sent here (it would need a CORS preflight, which this server never approves). */
    if (!strcmp(method, "POST")) {
        int ok = 0;
        for (char *l = strstr(hdr, "\r\n"); l && l < hdr + hend - 2; l = strstr(l + 2, "\r\n"))
            if (!strncasecmp(l + 2, "X-TBox:", 7)) { ok = 1; break; }
        if (!ok) {
            reply_text(s, 403, "Forbidden", "Missing X-TBox header (requests must come from the OrbisKit page).");
            sceNetSocketClose(s);
            return NULL;
        }
    }

    if (!strcmp(method, "GET") && !strcmp(target, "/")) {
        /* "/" alone is the dashboard; "/?p=..." (every folder link) is the file browser */
        if (strstr(query, "p=")) send_listing(s, p);
        else send_dashboard(s);
    } else if (!strcmp(method, "GET") && !strcmp(target, "/api/status")) {
        send_status(s);
    } else if (!strcmp(method, "GET") && !strcmp(target, "/log")) {
        send_logpage(s);
    } else if (!strcmp(method, "GET") && !strcmp(target, "/settings")) {
        send_settings_page(s);
    } else if (!strcmp(method, "GET") && !strcmp(target, "/api/settings")) {
        send_settings_json(s);
    } else if (!strcmp(method, "POST") && !strcmp(target, "/api/set")) {
        char k[32], v[64];
        get_param(query, "k", k, sizeof k);
        get_param(query, "v", v, sizeof v);
        receive_setting(s, k, v);
    } else if (!strcmp(method, "GET") && !strcmp(target, "/dl")) {
        send_file(s, p);
    } else if (!strcmp(method, "POST") && !strcmp(target, "/up")) {
        char sub[1024];
        get_param(query, "d", sub, sizeof sub);
        if (strlen(sub) >= sizeof sub - 1) reply_text(s, 400, "Bad Request", "Sub-folder path too long.");
        else receive_file(s, p, n, sub, clen, hdr + hend, len - hend);
    } else if (!strcmp(method, "POST") && !strcmp(target, "/zipsel")) {
        receive_zipsel(s, p, clen, hdr + hend, len - hend);
    } else if (!strcmp(method, "GET") && !strcmp(target, "/search")) {
        char q[256];
        get_param(query, "q", q, sizeof q);
        send_search(s, p, q);
    } else if (!strcmp(method, "POST") && !strcmp(target, "/mkdir")) {
        char path[1024];
        if (!valid_name(n) || path_join(path, sizeof path, p, n)) reply_text(s, 400, "Bad Request", "Invalid folder name.");
        else if (mkdir(path, 0777) == 0) { reply_text(s, 200, "OK", "Created"); wlog("Created folder %s", n); }
        else reply_text(s, 500, "Error", strerror(errno));
    } else if (!strcmp(method, "GET") && !strcmp(target, "/zip")) {
        send_zip(s, p);
    } else if (!strcmp(method, "GET") && !strcmp(target, "/view")) {
        char range[64] = "";
        for (char *l = strstr(hdr, "\r\n"); l && l < hdr + hend - 2; l = strstr(l + 2, "\r\n"))
            if (!strncasecmp(l + 2, "Range:", 6)) {
                const char *v = l + 8;
                while (*v == ' ') v++;
                int k = 0;
                while (v[k] && v[k] != '\r' && k < (int)sizeof range - 1) { range[k] = v[k]; k++; }
                range[k] = 0;
            }
        send_view(s, p, range);
    } else if (!strcmp(method, "GET") && !strcmp(target, "/apps")) {
        send_apps(s);
    } else if (!strcmp(method, "GET") && !strcmp(target, "/gallery")) {
        send_gallery(s);
    } else if (!strcmp(method, "GET") && !strcmp(target, "/edit")) {
        send_editor(s, p);
    } else if (!strcmp(method, "POST") && !strcmp(target, "/save")) {
        receive_save(s, p, clen, hdr + hend, len - hend);
    } else if (!strcmp(method, "GET") && !strcmp(target, "/payload")) {
        send_payload_page(s);
    } else if (!strcmp(method, "POST") && !strcmp(target, "/payload")) {
        receive_payload(s, n, clen, hdr + hend, len - hend);
    } else if (!strcmp(method, "POST") && !strcmp(target, "/rename")) {
        char dst[1024], dir[1024];
        snprintf(dir, sizeof dir, "%s", p);
        char *sl = strrchr(dir, '/');
        if (sl == dir) dir[1] = 0; else if (sl) *sl = 0;
        struct kstat st;
        if (!valid_name(n) || protected_path(p) || path_join(dst, sizeof dst, dir, n))
            reply_text(s, 400, "Bad Request", "Invalid name or protected folder.");
        else if (k_lstat(dst, &st) == 0) reply_text(s, 409, "Conflict", "Something with that name already exists.");
        else if (rename(p, dst) == 0) { reply_text(s, 200, "OK", "Renamed"); wlog("Renamed %s -> %s", path_base(p), n); }
        else reply_text(s, 500, "Error", strerror(errno));
    } else if (!strcmp(method, "POST") && !strcmp(target, "/del")) {
        if (protected_path(p)) {
            reply_text(s, 403, "Forbidden", "That folder is protected.");
        } else if (remove_tree(p) == 0) {
            reply_text(s, 200, "OK", "Deleted");
            wlog("Deleted %s", path_base(p));
        } else {
            reply_text(s, 500, "Error", strerror(errno));
        }
    } else if (!strcmp(target, "/favicon.ico")) {
        reply_text(s, 404, "Not Found", "");
    } else {
        reply_text(s, 404, "Not Found", "Not found.");
    }
    sceNetSocketClose(s);
    return NULL;
}

/* conn_serve closes the socket on every path; this wrapper releases the connection slot */
static void *conn_thread(void *arg)
{
    conn_serve(arg);
    __sync_sub_and_fetch(&nconns, 1);
    return NULL;
}

static void *server_thread(void *arg)
{
    (void)arg;
    while (running) {
        int s = sceNetAccept(listen_sock, NULL, NULL);
        if (s < 0) { if (!running) break; sceKernelUsleep(100000); continue; }
        /* cap the threads: a flood of connections must not exhaust memory */
        if (__sync_add_and_fetch(&nconns, 1) > CONN_MAX) {
            __sync_sub_and_fetch(&nconns, 1);
            reply_text(s, 503, "Busy", "Too many connections. Try again in a moment.");
            sceNetSocketClose(s);
            continue;
        }
        if (thread_start(conn_thread, (void *)(intptr_t)s, STACK_NET, 1) != 0) {
            __sync_sub_and_fetch(&nconns, 1);
            sceNetSocketClose(s);
        }
    }
    return NULL;
}

int web_start(void)
{
    if (running) return 0;
    signal(SIGPIPE, SIG_IGN);   /* a browser closing mid-download must not kill the app */
    int s = sceNetSocket("tbox_web", NET_AF_INET, NET_SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    sceNetSetsockopt(s, NET_SOL_SOCKET, NET_SO_REUSEADDR, &one, sizeof one);
    NetSockaddrIn a;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = NET_AF_INET;
    a.sin_port = sceNetHtons(WEB_PORT);
    if (sceNetBind(s, &a, sizeof a) < 0 || sceNetListen(s, 16) < 0) {
        sceNetSocketClose(s);
        return -1;
    }
    listen_sock = s;
    running = 1;
    if (thread_start(server_thread, NULL, STACK_NET, 1) != 0) {
        running = 0;
        sceNetSocketClose(s);
        listen_sock = -1;
        return -1;
    }
    wlog("Web server started on port %d", WEB_PORT);
    return 0;
}

void web_stop(void)
{
    if (!running) return;
    running = 0;
    sceNetSocketAbort(listen_sock, 0);
    sceNetSocketClose(listen_sock);
    listen_sock = -1;
    wlog("Web server stopped");
}
