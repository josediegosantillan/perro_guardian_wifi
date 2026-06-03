#include "web_server.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_state.h"
#include "relay_control.h"
#include "router_watchdog.h"
#include "telegram_bot.h"
#include "wifi_config_portal.h"
#include "wifi_credentials.h"
#include "wifi_manager.h"

#define WEB_JSON_MAX 768
#define WEB_WIFI_SCAN_MAX 12
#define WEB_TELEGRAM_TEST_TASK_STACK 8192
#define WEB_TELEGRAM_TEST_TASK_PRIORITY 3

static const char *TAG = "web_server";
static httpd_handle_t s_server;

static void telegram_test_task(void *arg)
{
    char *message = (char *)arg;
    esp_err_t err = telegram_bot_send_test_message(message != NULL ? message : "Prueba desde PERRO_GUARDIAN_WIFI");
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Telegram test enviado correctamente");
    } else {
        ESP_LOGE(TAG, "Telegram test fallo: %s", esp_err_to_name(err));
    }
    free(message);
    vTaskDelete(NULL);
}

static const char INDEX_HTML[] =
"<!doctype html><html lang='es'><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Perro Guardian WiFi</title><style>"
"*{box-sizing:border-box;margin:0;padding:0}"
":root{--bg:#121a24;--s1:#182333;--s2:#223246;--acc:#7dd3fc;--ok:#86efac;--bad:#fca5a5;--warn:#fcd34d;--tx:#e5edf6;--tx2:#91a4bb;font-family:'Segoe UI',Arial,sans-serif;color:var(--tx);background:var(--bg)}"
"body{min-height:100vh}"
"header{background:linear-gradient(135deg,#13263a,#20364d);padding:14px 16px;border-bottom:1px solid rgba(125,211,252,.18)}"
".hd{display:flex;align-items:center;gap:12px;max-width:760px;margin:auto}"
".logo{font-size:38px;line-height:1;filter:drop-shadow(0 2px 6px rgba(125,211,252,.24))}"
".htx h1{font-size:20px;font-weight:800;color:var(--acc);letter-spacing:-.3px}.htx p{font-size:11px;color:var(--tx2);margin-top:2px}"
".dot{width:11px;height:11px;border-radius:50%;background:var(--ok);box-shadow:0 0 10px var(--ok);margin-left:auto;transition:background .5s,box-shadow .5s;animation:pulse 2s infinite}"
"@keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}"
".tabs{display:flex;gap:4px;overflow-x:auto;padding:10px 12px;background:rgba(5,11,18,.35);border-bottom:1px solid rgba(255,255,255,.06)}"
".tabs button{border:0;background:transparent;color:var(--tx2);padding:8px 13px;border-radius:20px;font-weight:600;font-size:13px;white-space:nowrap;cursor:pointer;transition:all .15s}"
".tabs button.active{background:#93c5fd;color:#102033;box-shadow:0 2px 10px rgba(147,197,253,.22)}"
".tabs button:hover:not(.active){background:rgba(255,255,255,.07);color:var(--tx)}"
"main{padding:12px;max-width:760px;margin:auto}.page{display:none}.page.active{display:block}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(138px,1fr));gap:10px}"
".card{background:var(--s1);border:1px solid rgba(255,255,255,.07);border-radius:14px;padding:14px;transition:border-color .2s,transform .1s}"
".card:hover{transform:translateY(-1px)}"
".card.ok-c{border-color:rgba(134,239,172,.28);background:linear-gradient(150deg,#13271c,var(--s1))}"
".card.bad-c{border-color:rgba(252,165,165,.28);background:linear-gradient(150deg,#2b171b,var(--s1))}"
".card.warn-c{border-color:rgba(252,211,77,.28);background:linear-gradient(150deg,#2a2414,var(--s1))}"
".card.ok-c .cv{color:var(--ok)}.card.bad-c .cv{color:var(--bad)}.card.warn-c .cv{color:var(--warn)}"
".ci{font-size:26px;margin-bottom:6px}.cv{font-size:17px;font-weight:700;color:var(--acc);margin-top:2px;word-break:break-all}"
".cl{font-size:10px;color:var(--tx2);text-transform:uppercase;letter-spacing:.8px;margin-top:5px}"
".sig{display:inline-flex;align-items:flex-end;gap:2px;height:14px;vertical-align:middle;margin-left:6px}"
".sb{background:rgba(255,255,255,.15);border-radius:2px;width:4px}.sb.on{background:var(--ok)}"
"button,input{font-size:15px}"
"button{padding:11px 14px;margin:4px 0;border:0;border-radius:8px;background:#93c5fd;color:#102033;font-weight:700;cursor:pointer;transition:filter .1s,transform .08s;box-shadow:0 2px 8px rgba(147,197,253,.18)}"
"button:hover{filter:brightness(1.1)}button:active,button.pressed{transform:scale(.97);filter:brightness(.82)}"
"button:disabled{background:#253547;color:#3d5568;cursor:wait;filter:none;box-shadow:none}"
"button.working::after{content:' ⏳'}"
"button.danger{background:#b91c1c;box-shadow:0 2px 8px rgba(185,28,28,.18);color:#fff}"
"button.secondary{background:#3a4a60;box-shadow:none;color:var(--tx)}"
"input{width:100%;padding:10px;margin:4px 0 10px;border:1px solid rgba(255,255,255,.1);border-radius:8px;background:var(--s2);color:var(--tx)}"
"label{display:block;font-size:11px;font-weight:600;color:var(--tx2);margin-top:8px;text-transform:uppercase;letter-spacing:.5px}"
".row{display:flex;gap:8px;flex-wrap:wrap}.row button{flex:1;min-width:130px}"
"pre{white-space:pre-wrap;background:#0d1520;color:#bae6fd;border-radius:10px;padding:12px;overflow:auto;font-size:12px;border:1px solid rgba(125,211,252,.12);margin-top:10px}"
".hint{background:rgba(252,211,77,.08);border:1px solid rgba(252,211,77,.22);padding:10px;border-radius:8px;font-size:13px;color:#fde68a;margin:8px 0}"
".sect{background:var(--s1);border-radius:14px;padding:14px;margin-bottom:10px;border:1px solid rgba(255,255,255,.07)}"
"h2{font-size:15px;font-weight:700;color:var(--acc);margin-bottom:12px}"
".toast{position:fixed;left:12px;right:12px;bottom:14px;background:#20364d;color:var(--tx);padding:13px;border-radius:10px;box-shadow:0 8px 30px rgba(0,0,0,.45);z-index:9;text-align:center;font-weight:700;border:1px solid rgba(255,255,255,.1)}"
".toast.okt{background:#16442b;border-color:rgba(134,239,172,.28);color:#bbf7d0}"
".toast.err{background:#5f1f24;border-color:rgba(252,165,165,.28);color:#fecaca}"
"</style></head><body>"
"<header><div class='hd'>"
"<span class='logo'>&#x1F415;</span>"
"<div class='htx'><h1>Perro Guardian WiFi</h1><p>Monitor de conectividad ESP32</p></div>"
"<div class='dot' id='hDot'></div>"
"</div></header>"
"<nav class='tabs' id='tabs'></nav>"
"<main>"
"<section id='dash' class='page active'>"
"<div class='row' style='margin-bottom:10px'><button onclick='loadStatus(event)'>&#8635; Actualizar</button></div>"
"<div id='cards' class='grid'></div>"
"</section>"
"<section id='actions' class='page'><div class='sect'><h2>&#x26A1; Acciones</h2><div class='row'>"
"<button class='danger' onclick=\"cmd('router_power_cycle','Cortar la alimentacion del router unos segundos. Continuar?',event)\">&#x1F50C; Reiniciar router</button>"
"<button onclick=\"cmd('force_wifi_portal','El ESP32 abrira el portal WiFi al reiniciar. Continuar?',event)\">&#x1F4E1; Forzar portal</button>"
"<button class='danger' onclick=\"cmd('erase_wifi_credentials','Borrar credenciales WiFi. Continuar?',event)\">&#x1F5D1; Borrar WiFi</button>"
"<button class='danger' onclick=\"cmd('esp_restart','Reiniciar ESP32 ahora?',event)\">&#x1F504; Reiniciar ESP32</button>"
"<button class='secondary' onclick=\"cmd('clear_stats','Limpiar contadores estadisticos?',event)\">&#x1F4CA; Limpiar stats</button>"
"</div></div></section>"
"<section id='config' class='page'><div class='sect'><h2>&#x2699;&#xFE0F; Configuracion Watchdog</h2>"
"<div id='cfg'></div>"
"<div class='row'><button onclick='loadConfig(event)'>&#128260; Leer</button><button onclick='saveConfig(event)'>&#x1F4BE; Guardar</button></div>"
"<p class='hint'>Los parametros son de compilacion en esta version — se muestran como referencia.</p>"
"</div></section>"
"<section id='wifi' class='page'><div class='sect'><h2>&#x1F4F6; Configuracion WiFi</h2>"
"<button onclick='scanWifi(event)'>&#x1F50D; Escanear redes</button>"
"<div id='nets' style='margin:8px 0'></div>"
"<label>SSID</label><input id='wifiSsid' placeholder='Nombre de la red'>"
"<label>Contrasena</label><input id='wifiPass' type='password' placeholder='Contrasena WiFi'>"
"<div class='row'><button class='secondary' onclick='togglePass()'>&#x1F441; Ver/Ocultar</button><button onclick='saveWifi(event)'>&#x1F4BE; Guardar WiFi</button></div>"
"<p class='hint'>Al guardar, el ESP32 se reiniciara para conectar a la nueva red.</p>"
"</div></section>"
"<section id='telegram' class='page'><div class='sect'><h2>&#x1F4E8; Telegram</h2>"
"<p class='hint'>Telegram requiere conexion a internet activa para funcionar.</p>"
"<label><input id='tgEnabled' type='checkbox'> Habilitar Telegram</label>"
"<label>Bot Token</label><input id='tgToken' type='password' placeholder='Token del bot de Telegram'>"
"<label>Chat ID autorizado</label><input id='tgChat' placeholder='ID numerico del chat'>"
"<label><input id='tgNet' type='checkbox'> Notificar caida de internet</label>"
"<label><input id='tgReset' type='checkbox'> Notificar reset del router</label>"
"<label><input id='tgCooldown' type='checkbox'> Notificar cooldown activo</label>"
"<label><input id='tgCommands' type='checkbox'> Permitir comandos remotos</label>"
"<div class='row'><button onclick='loadTelegram(event)'>&#128260; Leer</button><button onclick='saveTelegram(event)'>&#x1F4BE; Guardar</button><button onclick='testTelegram(event)'>&#x1F4E4; Probar</button></div>"
"<p style='font-size:12px;color:var(--tx2);margin-top:8px'>Comandos: /estado /ip /wifi /internet /reset_router /portal /cooldown /ayuda</p>"
"</div></section>"
"<section id='logs' class='page'><div class='sect'><h2>&#x1F4CB; Logs</h2>"
"<div class='row'><button onclick='loadLogs(event)'>&#8635; Actualizar</button>"
"<button class='secondary' onclick=\"logBox.textContent='Sin logs.';toast('Limpiado','okt')\">&#x1F5D1; Limpiar</button></div>"
"<pre id='logBox'>Sin logs cargados.</pre>"
"</div></section>"
"</main>"
"<script>"
"const pages=['dash','telegram','actions','config','wifi','logs'];"
"const tabIx={dash:'&#x1F4CA;',actions:'&#x26A1;',config:'&#x2699;&#xFE0F;',wifi:'&#x1F4F6;',telegram:'&#x1F4E8;',logs:'&#x1F4CB;'};"
"let stateTimer=null;"
"function tab(id){"
"pages.forEach(p=>document.getElementById(p).classList.toggle('active',p===id));"
"[...document.querySelectorAll('.tabs button')].forEach(b=>b.classList.toggle('active',b.dataset.p===id));"
"if(id==='dash')loadStatus();if(id==='config')loadConfig();if(id==='telegram')loadTelegram();"
"}"
"document.getElementById('tabs').innerHTML=pages.map(p=>`<button data-p='${p}' onclick=\"tab('${p}')\">${tabIx[p]} ${p.toUpperCase()}</button>`).join('');"
"document.querySelector('.tabs button').classList.add('active');"
"function toast(m,t=''){let e=document.getElementById('_t')||document.body.appendChild(Object.assign(document.createElement('div'),{id:'_t'}));e.className='toast '+t;e.textContent=m;e.style.display='block';clearTimeout(window._tt);window._tt=setTimeout(()=>e.style.display='none',2800)}"
"async function busy(lbl,fn,btn){let bs=[...document.querySelectorAll('button')];if(btn)btn.classList.add('pressed','working');bs.forEach(b=>b.disabled=true);toast(lbl||'...');try{let r=await fn();toast('Listo ✓','okt');return r}catch(e){toast(e.message||'Error','err');throw e}finally{bs.forEach(b=>b.disabled=false);if(btn)btn.classList.remove('pressed','working')}}"
"async function api(path,opt={}){let timeout=opt.timeoutMs||12000;delete opt.timeoutMs;let c=new AbortController(),t=setTimeout(()=>c.abort('timeout'),timeout);try{let r=await fetch(path,{...opt,signal:c.signal,headers:{'Content-Type':'application/json',...(opt.headers||{})}});let tx=await r.text(),j=tx?JSON.parse(tx):{};if(!r.ok)throw Error(j.message||r.statusText);return j}catch(e){if(e.name==='AbortError'||e==='timeout')throw Error('Timeout: el ESP32 no respondio a tiempo. Revisar internet/DNS/Telegram.');throw e}finally{clearTimeout(t)}}"
"function fmt_up(s){let h=Math.floor(s/3600),m=Math.floor(s%3600/60),sc=s%60;return h?`${h}h ${m}m`:`${m}m ${sc}s`}"
"function sig_html(rssi){let n=rssi>-55?4:rssi>-65?3:rssi>-75?2:1;return `<span class='sig'>${[1,2,3,4].map(i=>`<span class='sb${i<=n?' on':''}' style='height:${i*3+2}px'></span>`).join('')}</span>`}"
"function card(ico,lbl,val,cls=''){return `<div class='card ${cls}'><div class='ci'>${ico}</div><div class='cv'>${val}</div><div class='cl'>${lbl}</div></div>`}"
"async function loadStatus(ev){"
"try{"
"let run=async()=>{"
"let s=await api('/api/status'),wc=s.wifiConnected,io=s.internetOk,rb=s.routerRebooting;"
"let d=document.getElementById('hDot'),col=io?'var(--ok)':wc?'var(--warn)':'var(--bad)';"
"d.style.background=col;d.style.boxShadow='0 0 10px '+col;"
"let ssidTx=wc?(s.ssid||'?')+sig_html(s.rssiDbm||0):'Desconectado';"
"cards.innerHTML="
"card(wc?'&#x1F4F6;':'&#x1F4F5;','WiFi',ssidTx,wc?'ok-c':'bad-c')+"
"card(io?'&#x1F310;':'&#x1F534;','Internet',io?'Online':'Sin internet',io?'ok-c':'bad-c')+"
"card(rb?'&#x1F504;':'&#x1F50C;','Modem',rb?'Reiniciando':'Activo',rb?'warn-c':'ok-c')+"
"card(s.cooldown?'&#x2744;&#xFE0F;':'&#x1F7E2;','Cooldown',s.cooldown?'Activo':'Normal',s.cooldown?'warn-c':'ok-c')+"
"card('&#x1F4CD;','IP',s.ip||'0.0.0.0','')+"
"card('&#x1F4F6;','RSSI',(s.rssiDbm||0)+' dBm','')+"
"card('&#x23F1;&#xFE0F;','Uptime',fmt_up(s.uptimeSec||0),'')+"
"card('&#x1F504;','Reinicios',s.totalReboots||0,'')+"
"card('&#x26A0;&#xFE0F;','Fallos',s.consecutiveFailures||0,s.consecutiveFailures>0?'warn-c':'ok-c')+"
"card('&#x1F6AB;','Limite hit',s.limitHits||0,s.limitHits>0?'warn-c':'ok-c')+"
"card(s.testMode?'&#x1F9EA;':'&#x2699;&#xFE0F;','Modo',s.testMode?'TEST':'LIVE',s.testMode?'warn-c':'ok-c')+"
"card('&#x1F4DD;','Ultimo motivo',s.lastReason||'-','')"
"};"
"ev?await busy('Actualizando...',run,ev.target):await run()"
"}catch(e){cards.innerHTML=`<div class='card bad-c'><div class='ci'>&#x274C;</div><div class='cv'>${e.message}</div><div class='cl'>Error de conexion</div></div>`}"
"}"
"async function cmd(c,msg,ev){if(msg&&!confirm(msg))return;try{let r=await busy('Enviando...',()=>api('/api/command',{method:'POST',body:JSON.stringify({cmd:c})}),ev&&ev.target);alert(r.message||'OK')}catch(e){alert(e.message)}}"
"let cfgF=['wifiConnectTimeoutMs','watchdogCheckIntervalMs','internetCheckTimeoutMs','maxConsecutiveFailures','routerPowerOffMs','routerBootWaitMs','powerCycleMaxPerWindow','powerCycleWindowMs','cooldownAfterLimitMs','internetCheckUrl1','internetCheckUrl2'];"
"function renderCfg(c){cfg.innerHTML=cfgF.map(k=>`<label>${k}</label><input id='c_${k}' value='${c[k]===undefined?'':c[k]}'>`).join('')+`<label><input id='c_strictHttp204' type='checkbox' ${c.strictHttp204?'checked':''}> strictHttp204</label><label><input id='c_testMode' type='checkbox' ${c.testMode?'checked':''}> testMode</label>`}"
"async function loadConfig(ev){try{await busy('Leyendo...',async()=>renderCfg(await api('/api/config')),ev&&ev.target)}catch(e){cfg.innerHTML=e.message}}"
"async function saveConfig(ev){let o={};cfgF.forEach(k=>o[k]=document.getElementById('c_'+k).value);o.strictHttp204=c_strictHttp204.checked;o.testMode=c_testMode.checked;try{let r=await busy('Guardando...',()=>api('/api/config',{method:'POST',body:JSON.stringify(o)}),ev&&ev.target);alert(r.message)}catch(e){alert(e.message)}}"
"async function scanWifi(ev){try{nets.innerHTML='<p class=\"hint\">Escaneando...</p>';let j=await busy('Escaneando WiFi...',()=>api('/api/wifi/scan'),ev&&ev.target);nets.innerHTML=j.networks.map(n=>`<button class='secondary' style='margin:2px 0' onclick=\"wifiSsid.value='${n.ssid}';toast('Seleccionado: ${n.ssid}','okt')\">&#x1F4F6; ${n.ssid} (${n.rssi} dBm)</button>`).join('')}catch(e){nets.textContent=e.message}}"
"function togglePass(){wifiPass.type=wifiPass.type==='password'?'text':'password';toast(wifiPass.type==='password'?'Password oculto':'Password visible','okt')}"
"async function saveWifi(ev){try{let r=await busy('Guardando WiFi...',()=>api('/api/wifi/credentials',{method:'POST',body:JSON.stringify({ssid:wifiSsid.value,password:wifiPass.value})}),ev&&ev.target);alert(r.message)}catch(e){alert(e.message)}}"
"async function loadTelegram(ev){try{await busy('Leyendo Telegram...',async()=>{let t=await api('/api/telegram/config');tgEnabled.checked=t.enabled;tgChat.value=t.chatId||'';tgToken.value='';tgNet.checked=t.notifyOnInternetFail;tgReset.checked=t.notifyOnRouterReset;tgCooldown.checked=t.notifyOnCooldown;tgCommands.checked=t.allowCommands},ev&&ev.target)}catch(e){alert(e.message)}}"
"async function saveTelegram(ev){if(tgCommands.checked&&!confirm('Los comandos permiten reiniciar el router. Continuar?'))return;let b={enabled:tgEnabled.checked,botToken:tgToken.value,chatId:tgChat.value,notifyOnInternetFail:tgNet.checked,notifyOnRouterReset:tgReset.checked,notifyOnCooldown:tgCooldown.checked,allowCommands:tgCommands.checked};try{let r=await busy('Guardando Telegram...',()=>api('/api/telegram/config',{method:'POST',body:JSON.stringify(b)}),ev&&ev.target);alert(r.message)}catch(e){alert(e.message)}}"
"async function testTelegram(ev){try{let r=await busy('Enviando prueba...',()=>api('/api/telegram/test',{method:'POST',body:JSON.stringify({message:'Prueba desde Perro Guardian WiFi'}),timeoutMs:18000}),ev&&ev.target);alert(r.message)}catch(e){alert(e.message)}}"
"async function loadLogs(ev){try{await busy('Actualizando logs...',async()=>{let l=await api('/api/logs');logBox.textContent=l.logs.map(x=>`[${x.timeSec}s] ${x.level} ${x.tag}: ${x.message}`).join('\\n')},ev&&ev.target)}catch(e){logBox.textContent=e.message}}"
"loadStatus();stateTimer=setInterval(loadStatus,3000);"
"</script></body></html>";

static bool json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pattern[48] = {0};
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (pos == NULL) {
        return false;
    }
    pos = strchr(pos, ':');
    if (pos == NULL) {
        return false;
    }
    while (*pos != '\0' && *pos != '"') {
        pos++;
    }
    if (*pos != '"') {
        return false;
    }
    pos++;
    const char *end = strchr(pos, '"');
    if (end == NULL) {
        return false;
    }
    size_t len = (size_t)(end - pos);
    if (len >= out_len) {
        len = out_len - 1;
    }
    memcpy(out, pos, len);
    out[len] = '\0';
    return true;
}

static bool json_get_bool(const char *json, const char *key, bool default_value)
{
    char pattern[48] = {0};
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (pos == NULL) {
        return default_value;
    }
    pos = strchr(pos, ':');
    if (pos == NULL) {
        return default_value;
    }
    pos++;
    while (isspace((unsigned char)*pos)) {
        pos++;
    }
    return strncmp(pos, "true", 4) == 0;
}

static esp_err_t read_body(httpd_req_t *req, char *body, size_t body_len)
{
    if (req->content_len <= 0 || req->content_len >= body_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    int remaining = req->content_len;
    int total = 0;
    while (remaining > 0) {
        int received = httpd_req_recv(req, body + total, remaining);
        if (received <= 0) {
            return ESP_FAIL;
        }
        total += received;
        remaining -= received;
    }
    body[total] = '\0';
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captive_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "");
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char ssid[33] = {0};
    char ip[16] = {0};
    int rssi = 0;
    wifi_manager_get_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);

    router_watchdog_status_t wd;
    router_watchdog_get_status(&wd);

    char json[640] = {0};
    snprintf(
        json,
        sizeof(json),
        "{\"wifiConnected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssiDbm\":%d,"
        "\"internetOk\":%s,\"cooldown\":%s,\"routerRebooting\":%s,\"relayActive\":%s,"
        "\"testMode\":%s,\"uptimeSec\":%lld,\"consecutiveFailures\":%d,"
        "\"totalReboots\":%lu,\"limitHits\":%lu,\"lastReason\":\"%s\",\"portalAvailable\":true}",
        wifi_manager_is_connected() ? "true" : "false",
        ssid,
        ip,
        rssi,
        app_state_test_bits(APP_STATE_INTERNET_OK_BIT) ? "true" : "false",
        app_state_test_bits(APP_STATE_COOLDOWN_BIT) ? "true" : "false",
        app_state_test_bits(APP_STATE_ROUTER_REBOOTING_BIT) ? "true" : "false",
        relay_control_is_rebooting() ? "true" : "false",
        APP_TEST_MODE ? "true" : "false",
        (long long)(esp_timer_get_time() / 1000000LL),
        wd.consecutive_failures,
        wd.total_reboots,
        wd.limit_hits,
        wd.last_reason);
    return send_json(req, json);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    char json[768] = {0};
    snprintf(
        json,
        sizeof(json),
        "{\"wifiConnectTimeoutMs\":%d,\"watchdogCheckIntervalMs\":%d,"
        "\"internetCheckTimeoutMs\":%d,\"maxConsecutiveFailures\":%d,"
        "\"routerPowerOffMs\":%d,\"routerBootWaitMs\":%d,"
        "\"powerCycleMaxPerWindow\":%d,\"powerCycleWindowMs\":%d,"
        "\"cooldownAfterLimitMs\":%d,\"strictHttp204\":%s,\"testMode\":%s,"
        "\"internetCheckUrl1\":\"%s\",\"internetCheckUrl2\":\"%s\"}",
        APP_WIFI_CONNECT_TIMEOUT_MS,
        APP_WATCHDOG_CHECK_INTERVAL_MS,
        APP_INTERNET_CHECK_TIMEOUT_MS,
        APP_WATCHDOG_MAX_FAILURES,
        APP_ROUTER_POWER_OFF_MS,
        APP_ROUTER_BOOT_WAIT_MS,
        APP_POWER_CYCLE_MAX_PER_WINDOW,
        APP_POWER_CYCLE_WINDOW_MS,
        APP_COOLDOWN_AFTER_LIMIT_MS,
        APP_STRICT_HTTP_204 ? "true" : "false",
        APP_TEST_MODE ? "true" : "false",
        APP_INTERNET_CHECK_URL_1,
        APP_INTERNET_CHECK_URL_2);
    return send_json(req, json);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char body[WEB_JSON_MAX] = {0};
    esp_err_t err = read_body(req, body, sizeof(body));
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON invalido");
        return err;
    }
    return send_json(req, "{\"ok\":true,\"message\":\"Configuracion recibida. Los parametros principales son de compilacion en esta version.\"}");
}

static esp_err_t command_post_handler(httpd_req_t *req)
{
    char body[WEB_JSON_MAX] = {0};
    char cmd[48] = {0};
    if (read_body(req, body, sizeof(body)) != ESP_OK || !json_get_string(body, "cmd", cmd, sizeof(cmd))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cmd requerido");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    const char *message = "Comando aceptado";

    if (strcmp(cmd, "router_power_cycle") == 0) {
        err = relay_control_request_power_cycle(APP_ROUTER_POWER_OFF_MS, APP_ROUTER_BOOT_WAIT_MS);
    } else if (strcmp(cmd, "force_wifi_portal") == 0) {
        err = wifi_config_portal_request_on_next_boot();
        message = "Portal programado. Reiniciando ESP32";
    } else if (strcmp(cmd, "erase_wifi_credentials") == 0) {
        err = wifi_credentials_erase();
        message = "Credenciales borradas. Reiniciando ESP32";
    } else if (strcmp(cmd, "esp_restart") == 0) {
        message = "Reiniciando ESP32";
    } else if (strcmp(cmd, "clear_stats") == 0) {
        err = router_watchdog_clear_stats();
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cmd desconocido");
        return ESP_FAIL;
    }

    char json[160] = {0};
    snprintf(json, sizeof(json), "{\"ok\":%s,\"cmd\":\"%s\",\"message\":\"%s\"}", err == ESP_OK ? "true" : "false", cmd, message);
    send_json(req, json);

    if (err == ESP_OK &&
        (strcmp(cmd, "force_wifi_portal") == 0 || strcmp(cmd, "erase_wifi_credentials") == 0 || strcmp(cmd, "esp_restart") == 0)) {
        vTaskDelay(pdMS_TO_TICKS(700));
        esp_restart();
    }
    return ESP_OK;
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return err;
    }

    if (mode == WIFI_MODE_AP) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
            return err;
        }
    } else if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi STA no disponible para escanear");
        return ESP_FAIL;
    }

    wifi_scan_config_t scan_config = {0};
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.show_hidden = false;
    scan_config.scan_time.active.min = 80;
    scan_config.scan_time.active.max = 180;

    err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Escaneo WiFi fallo: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return err;
    }

    uint16_t count = WEB_WIFI_SCAN_MAX;
    wifi_ap_record_t records[WEB_WIFI_SCAN_MAX] = {0};
    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudieron leer resultados WiFi: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return err;
    }

    char json[768] = {0};
    size_t written = strlcpy(json, "{\"networks\":[", sizeof(json));
    for (uint16_t i = 0; i < count; i++) {
        char item[96] = {0};
        snprintf(
            item,
            sizeof(item),
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\"}",
            i == 0 ? "" : ",",
            (const char *)records[i].ssid,
            records[i].rssi,
            records[i].authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA/WPA2");
        strlcpy(json + written, item, sizeof(json) - written);
        written = strlen(json);
    }
    strlcpy(json + written, "]}", sizeof(json) - written);
    return send_json(req, json);
}

static esp_err_t wifi_credentials_post_handler(httpd_req_t *req)
{
    char body[WEB_JSON_MAX] = {0};
    char ssid[WIFI_CREDENTIALS_MAX_SSID_LEN + 1] = {0};
    char password[WIFI_CREDENTIALS_MAX_PASSWORD_LEN + 1] = {0};
    if (read_body(req, body, sizeof(body)) != ESP_OK ||
        !json_get_string(body, "ssid", ssid, sizeof(ssid)) ||
        !json_get_string(body, "password", password, sizeof(password))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid/password requeridos");
        return ESP_FAIL;
    }

    esp_err_t err = wifi_credentials_save(ssid, password);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return err;
    }

    send_json(req, "{\"ok\":true,\"message\":\"Credenciales guardadas. El ESP32 se reiniciara.\"}");
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
    return ESP_OK;
}

static esp_err_t logs_get_handler(httpd_req_t *req)
{
    router_watchdog_status_t wd;
    router_watchdog_get_status(&wd);
    char json[256] = {0};
    snprintf(
        json,
        sizeof(json),
        "{\"logs\":[{\"timeSec\":%lld,\"level\":\"INFO\",\"tag\":\"router_watchdog\",\"message\":\"ultimo motivo: %s, reinicios: %lu\"}]}",
        (long long)(esp_timer_get_time() / 1000000LL),
        wd.last_reason,
        wd.total_reboots);
    return send_json(req, json);
}

static esp_err_t telegram_config_get_handler(httpd_req_t *req)
{
    telegram_bot_config_t config;
    telegram_bot_get_config(&config);
    char json[256] = {0};
    snprintf(
        json,
        sizeof(json),
        "{\"enabled\":%s,\"botTokenConfigured\":%s,\"chatId\":\"%s\","
        "\"notifyOnInternetFail\":%s,\"notifyOnRouterReset\":%s,"
        "\"notifyOnCooldown\":%s,\"allowCommands\":%s}",
        config.enabled ? "true" : "false",
        config.bot_token_configured ? "true" : "false",
        config.chat_id,
        config.notify_on_internet_fail ? "true" : "false",
        config.notify_on_router_reset ? "true" : "false",
        config.notify_on_cooldown ? "true" : "false",
        config.allow_commands ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t telegram_config_post_handler(httpd_req_t *req)
{
    char body[WEB_JSON_MAX] = {0};
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON invalido");
        return ESP_FAIL;
    }

    telegram_bot_config_t config;
    telegram_bot_get_config(&config);
    config.enabled = json_get_bool(body, "enabled", config.enabled);
    config.notify_on_internet_fail = json_get_bool(body, "notifyOnInternetFail", config.notify_on_internet_fail);
    config.notify_on_router_reset = json_get_bool(body, "notifyOnRouterReset", config.notify_on_router_reset);
    config.notify_on_cooldown = json_get_bool(body, "notifyOnCooldown", config.notify_on_cooldown);
    config.allow_commands = json_get_bool(body, "allowCommands", config.allow_commands);
    json_get_string(body, "chatId", config.chat_id, sizeof(config.chat_id));
    bool update_token = json_get_string(body, "botToken", config.bot_token, sizeof(config.bot_token)) && config.bot_token[0] != '\0';

    esp_err_t err = telegram_bot_save_config(&config, update_token);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return err;
    }
    return send_json(req, "{\"ok\":true,\"message\":\"Configuracion Telegram guardada\"}");
}

static esp_err_t telegram_test_post_handler(httpd_req_t *req)
{
    char body[WEB_JSON_MAX] = {0};
    char message[160] = "Prueba desde PERRO_GUARDIAN_WIFI";
    if (read_body(req, body, sizeof(body)) == ESP_OK) {
        json_get_string(body, "message", message, sizeof(message));
    }

    telegram_bot_config_t config;
    telegram_bot_get_config(&config);
    if (!config.enabled) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Telegram esta deshabilitado");
        return ESP_FAIL;
    }
    if (!config.bot_token_configured) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Falta guardar el Bot Token");
        return ESP_FAIL;
    }
    if (config.chat_id[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Falta guardar el Chat ID");
        return ESP_FAIL;
    }
    if (!wifi_manager_is_connected()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "El ESP32 no esta conectado al WiFi");
        return ESP_FAIL;
    }

    char *task_message = strdup(message);
    if (task_message == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Sin memoria para enviar prueba");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(
        telegram_test_task,
        "telegram_test",
        WEB_TELEGRAM_TEST_TASK_STACK,
        task_message,
        WEB_TELEGRAM_TEST_TASK_PRIORITY,
        NULL);
    if (ok != pdPASS) {
        free(task_message);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudo crear tarea Telegram");
        return ESP_ERR_NO_MEM;
    }

    return send_json(req, "{\"ok\":true,\"message\":\"Prueba Telegram enviada en segundo plano. Revisar Telegram y monitor serie.\"}");
}

static esp_err_t register_uri(httpd_handle_t server, const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t route = {
        .uri = uri,
        .method = method,
        .handler = handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(server, &route);
}

esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 24;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start fallo");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/", HTTP_GET, root_get_handler), TAG, "root");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/api/status", HTTP_GET, status_get_handler), TAG, "status");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/api/config", HTTP_GET, config_get_handler), TAG, "config get");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/api/config", HTTP_POST, config_post_handler), TAG, "config post");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/api/command", HTTP_POST, command_post_handler), TAG, "command");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/api/wifi/scan", HTTP_GET, wifi_scan_get_handler), TAG, "wifi scan");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/api/wifi/credentials", HTTP_POST, wifi_credentials_post_handler), TAG, "wifi creds");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/api/logs", HTTP_GET, logs_get_handler), TAG, "logs");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/api/telegram/config", HTTP_GET, telegram_config_get_handler), TAG, "telegram get");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/api/telegram/config", HTTP_POST, telegram_config_post_handler), TAG, "telegram post");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/api/telegram/test", HTTP_POST, telegram_test_post_handler), TAG, "telegram test");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/generate_204", HTTP_GET, captive_redirect_handler), TAG, "android captive");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/gen_204", HTTP_GET, captive_redirect_handler), TAG, "android captive gen");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/hotspot-detect.html", HTTP_GET, captive_redirect_handler), TAG, "ios captive");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/library/test/success.html", HTTP_GET, captive_redirect_handler), TAG, "ios captive success");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/connecttest.txt", HTTP_GET, captive_redirect_handler), TAG, "windows captive");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/ncsi.txt", HTTP_GET, captive_redirect_handler), TAG, "windows ncsi");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/redirect", HTTP_GET, captive_redirect_handler), TAG, "windows redirect");
    ESP_RETURN_ON_ERROR(register_uri(s_server, "/*", HTTP_GET, captive_get_handler), TAG, "captive wildcard");

    ESP_LOGI(TAG, "Servidor web iniciado en puerto 80");
    return ESP_OK;
}
