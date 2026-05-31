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

static const char *TAG = "web_server";
static httpd_handle_t s_server;

static const char INDEX_HTML[] =
"<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>PERRO_GUARDIAN_WIFI</title><style>"
":root{font-family:Arial,sans-serif;color:#17202a;background:#eef2f5}body{margin:0}header{background:#102a43;color:white;padding:14px 16px}"
"h1{font-size:20px;margin:0}.tabs{display:flex;gap:6px;overflow:auto;padding:8px;background:#d9e2ec}.tabs button{border:0;background:#bcccdc;padding:10px;border-radius:6px;font-weight:bold}"
".tabs button.active{background:#0b5fff;color:white}main{padding:12px;max-width:760px;margin:auto}.page{display:none}.page.active{display:block}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(145px,1fr));gap:8px}.card{background:white;border:1px solid #d0d7de;border-radius:8px;padding:10px}"
".label{font-size:12px;color:#52606d}.value{font-size:18px;font-weight:bold;margin-top:4px}.ok{color:#087f5b}.bad{color:#c92a2a}.warn{color:#b35c00}.info{color:#0b5fff}"
"button,input,select{font-size:16px}button{padding:11px;margin:5px 0;border:0;border-radius:6px;background:#0b5fff;color:white;font-weight:bold;box-shadow:0 2px 5px #9fb3c8;transition:transform .08s,filter .12s,box-shadow .12s,outline .12s;cursor:pointer}"
"button:hover{filter:brightness(1.14);outline:3px solid #9ec5ff;box-shadow:0 5px 12px #829ab1}button:active,button.pressed{transform:translateY(2px) scale(.98);filter:brightness(.86);box-shadow:inset 0 2px 5px #102a43}button:disabled{background:#829ab1;cursor:wait;filter:none;box-shadow:none}"
"button.working::after{content:' ...';font-weight:bold}.toast{position:fixed;left:12px;right:12px;bottom:14px;background:#102a43;color:white;padding:12px;border-radius:8px;box-shadow:0 6px 18px #52606d;z-index:9;text-align:center;font-weight:bold}.toast.okt{background:#087f5b}.toast.err{background:#a32121}"
"button.danger{background:#a32121}button.secondary{background:#52606d}input{width:100%;box-sizing:border-box;padding:10px;margin:4px 0 10px;border:1px solid #bcccdc;border-radius:6px}"
"label{display:block;font-size:13px;font-weight:bold;margin-top:8px}.row{display:flex;gap:8px;flex-wrap:wrap}.row button{flex:1;min-width:130px}"
"pre{white-space:pre-wrap;background:#17202a;color:#d9e2ec;border-radius:6px;padding:10px;overflow:auto}.hint{background:#fff3bf;border:1px solid #ffe066;padding:10px;border-radius:6px}"
"</style></head><body><header><h1>PERRO_GUARDIAN_WIFI</h1></header>"
"<nav class='tabs' id='tabs'></nav><main>"
"<section id='dash' class='page active'><div class='row'><button onclick='loadStatus(event)'>Actualizar</button></div><div id='cards' class='grid'></div></section>"
"<section id='actions' class='page'><div class='card'><h2>Acciones</h2><div class='row'>"
"<button class='danger' onclick=\"cmd('router_power_cycle','Esto cortara la alimentacion del router durante unos segundos. Continuar?',event)\">Reiniciar router</button>"
"<button onclick=\"cmd('force_wifi_portal','El ESP32 reiniciara y abrira el portal. Continuar?',event)\">Forzar portal</button>"
"<button class='danger' onclick=\"cmd('erase_wifi_credentials','Esto borrara las credenciales WiFi del ESP32. Continuar?',event)\">Borrar WiFi</button>"
"<button class='danger' onclick=\"cmd('esp_restart','Reiniciar ESP32?',event)\">Reiniciar ESP32</button>"
"<button class='secondary' onclick=\"cmd('clear_stats','Limpiar contadores?',event)\">Limpiar contadores</button></div></div></section>"
"<section id='config' class='page'><div class='card'><h2>Configuracion watchdog</h2><div id='cfg'></div><div class='row'><button onclick='loadConfig(event)'>Leer</button><button onclick='saveConfig(event)'>Guardar</button></div><p class='hint'>Algunos parametros se guardan solo si el firmware los implementa como dinamicos. Esta version expone los valores actuales.</p></div></section>"
"<section id='wifi' class='page'><div class='card'><h2>WiFi</h2><button onclick='scanWifi(event)'>Escanear redes</button><div id='nets'></div><label>SSID</label><input id='wifiSsid'><label>Password</label><input id='wifiPass' type='password'><div class='row'><button onclick='togglePass()'>Mostrar/Ocultar</button><button onclick='saveWifi(event)'>Guardar WiFi</button></div><p class='hint'>Al guardar credenciales, el ESP32 puede reiniciarse y cambiar de IP.</p></div></section>"
"<section id='telegram' class='page'><div class='card'><h2>Telegram</h2><p class='hint'>Telegram requiere internet. Si el router esta sin internet, Telegram no funcionara.</p>"
"<label><input id='tgEnabled' type='checkbox'> Habilitar Telegram</label><label>Bot token</label><input id='tgToken' type='password'><label>Chat ID autorizado</label><input id='tgChat'>"
"<label><input id='tgNet' type='checkbox'> Avisar caida de internet</label><label><input id='tgReset' type='checkbox'> Avisar reset del router</label><label><input id='tgCooldown' type='checkbox'> Avisar cooldown</label><label><input id='tgCommands' type='checkbox'> Permitir comandos por Telegram</label>"
"<div class='row'><button onclick='loadTelegram(event)'>Leer</button><button onclick='saveTelegram(event)'>Guardar</button><button onclick='testTelegram(event)'>Probar</button></div><p>Comandos: /estado /ip /wifi /internet /reset_router /portal /cooldown /ayuda</p></div></section>"
"<section id='logs' class='page'><div class='card'><h2>Logs</h2><div class='row'><button onclick='loadLogs(event)'>Actualizar logs</button><button class='secondary' onclick=\"document.getElementById('logBox').textContent='';toast('Vista de logs limpiada','okt')\">Limpiar vista</button></div><pre id='logBox'></pre></div></section>"
"</main><script>"
"const pages=['dash','actions','config','wifi','telegram','logs'];let stateTimer=null;"
"function tab(id){pages.forEach(p=>document.getElementById(p).classList.toggle('active',p===id));[...document.querySelectorAll('.tabs button')].forEach(b=>b.classList.toggle('active',b.dataset.p===id));if(id==='dash')loadStatus();if(id==='config')loadConfig();if(id==='telegram')loadTelegram();}"
"document.getElementById('tabs').innerHTML=pages.map(p=>`<button data-p='${p}' onclick=\"tab('${p}')\">${p.toUpperCase()}</button>`).join('');document.querySelector('.tabs button').classList.add('active');"
"function toast(m,t=''){let e=document.getElementById('toast')||document.body.appendChild(Object.assign(document.createElement('div'),{id:'toast'}));e.className='toast '+t;e.textContent=m;e.style.display='block';clearTimeout(window.toastT);window.toastT=setTimeout(()=>e.style.display='none',2600)}"
"async function busy(label,fn,btn){let bs=[...document.querySelectorAll('button')];if(btn){btn.classList.add('pressed','working')}bs.forEach(b=>b.disabled=true);toast(label||'Trabajando...');try{let r=await fn();toast('Listo','okt');return r}catch(e){toast(e.message||'Error','err');throw e}finally{bs.forEach(b=>b.disabled=false);if(btn){btn.classList.remove('pressed','working')}}}"
"async function api(path,opt={}){let c=new AbortController();let t=setTimeout(()=>c.abort(),8000);try{let r=await fetch(path,{...opt,signal:c.signal,headers:{'Content-Type':'application/json',...(opt.headers||{})}});let tx=await r.text();let j=tx?JSON.parse(tx):{};if(!r.ok)throw Error(j.message||r.statusText);return j;}finally{clearTimeout(t)}}"
"function cls(v){return v?'ok':'bad'}function yn(v){return v?'SI':'NO'}function card(k,v,c='info'){return `<div class='card'><div class='label'>${k}</div><div class='value ${c}'>${v}</div></div>`}"
"async function loadStatus(ev){try{let run=async()=>{let s=await api('/api/status');cards.innerHTML=card('WiFi',yn(s.wifiConnected),cls(s.wifiConnected))+card('SSID',s.ssid||'-')+card('IP',s.ip||'-')+card('RSSI',s.rssiDbm||0)+card('Internet',yn(s.internetOk),cls(s.internetOk))+card('Router reset',yn(s.routerRebooting),s.routerRebooting?'warn':'ok')+card('Rele',yn(s.relayActive),s.relayActive?'warn':'ok')+card('Cooldown',yn(s.cooldown),s.cooldown?'warn':'ok')+card('TEST_MODE',yn(s.testMode),s.testMode?'warn':'ok')+card('Uptime',s.uptimeSec+' s')+card('Fallos',s.consecutiveFailures)+card('Reinicios',s.totalReboots)+card('Limites',s.limitHits)+card('Ultimo motivo',s.lastReason||'-')};ev?await busy('Actualizando estado...',run,ev.target):await run()}catch(e){cards.innerHTML=`<div class='card bad'>${e.message}</div>`}}"
"async function cmd(c,msg,ev){if(msg&&!confirm(msg))return;try{let r=await busy('Enviando comando...',()=>api('/api/command',{method:'POST',body:JSON.stringify({cmd:c})}),ev&&ev.target);alert(r.message||'OK')}catch(e){alert(e.message)}}"
"let cfgFields=['wifiConnectTimeoutMs','watchdogCheckIntervalMs','internetCheckTimeoutMs','maxConsecutiveFailures','routerPowerOffMs','routerBootWaitMs','powerCycleMaxPerWindow','powerCycleWindowMs','cooldownAfterLimitMs','internetCheckUrl1','internetCheckUrl2'];"
"function renderCfg(c){cfg.innerHTML=cfgFields.map(k=>`<label>${k}</label><input id='c_${k}' value='${c[k]===undefined?'':c[k]}'>`).join('')+`<label><input id='c_strictHttp204' type='checkbox' ${c.strictHttp204?'checked':''}> strictHttp204</label><label><input id='c_testMode' type='checkbox' ${c.testMode?'checked':''}> testMode</label>`}"
"async function loadConfig(ev){try{await busy('Leyendo configuracion...',async()=>renderCfg(await api('/api/config')),ev&&ev.target)}catch(e){cfg.innerHTML=e.message}}"
"async function saveConfig(ev){let o={};cfgFields.forEach(k=>o[k]=document.getElementById('c_'+k).value);o.strictHttp204=c_strictHttp204.checked;o.testMode=c_testMode.checked;try{let r=await busy('Guardando configuracion...',()=>api('/api/config',{method:'POST',body:JSON.stringify(o)}),ev&&ev.target);alert(r.message)}catch(e){alert(e.message)}}"
"async function scanWifi(ev){try{nets.innerHTML='<p class=\"hint\">Escaneando redes WiFi...</p>';let j=await busy('Escaneando redes WiFi...',()=>api('/api/wifi/scan'),ev&&ev.target);nets.innerHTML=j.networks.map(n=>`<button class='secondary' onclick=\"wifiSsid.value='${n.ssid}';toast('SSID seleccionado: ${n.ssid}','okt')\">${n.ssid} (${n.rssi})</button>`).join('')}catch(e){nets.textContent=e.message}}"
"function togglePass(){wifiPass.type=wifiPass.type==='password'?'text':'password';toast(wifiPass.type==='password'?'Password oculto':'Password visible','okt')}async function saveWifi(ev){try{let r=await busy('Guardando WiFi...',()=>api('/api/wifi/credentials',{method:'POST',body:JSON.stringify({ssid:wifiSsid.value,password:wifiPass.value})}),ev&&ev.target);alert(r.message)}catch(e){alert(e.message)}}"
"async function loadTelegram(ev){try{await busy('Leyendo Telegram...',async()=>{let t=await api('/api/telegram/config');tgEnabled.checked=t.enabled;tgChat.value=t.chatId||'';tgToken.value='';tgNet.checked=t.notifyOnInternetFail;tgReset.checked=t.notifyOnRouterReset;tgCooldown.checked=t.notifyOnCooldown;tgCommands.checked=t.allowCommands},ev&&ev.target)}catch(e){alert(e.message)}}"
"async function saveTelegram(ev){if(tgCommands.checked&&!confirm('Los comandos por Telegram pueden reiniciar el router. Continuar?'))return;let b={enabled:tgEnabled.checked,botToken:tgToken.value,chatId:tgChat.value,notifyOnInternetFail:tgNet.checked,notifyOnRouterReset:tgReset.checked,notifyOnCooldown:tgCooldown.checked,allowCommands:tgCommands.checked};try{let r=await busy('Guardando Telegram...',()=>api('/api/telegram/config',{method:'POST',body:JSON.stringify(b)}),ev&&ev.target);alert(r.message)}catch(e){alert(e.message)}}"
"async function testTelegram(ev){try{let r=await busy('Enviando prueba Telegram...',()=>api('/api/telegram/test',{method:'POST',body:JSON.stringify({message:'Prueba desde PERRO_GUARDIAN_WIFI'})}),ev&&ev.target);alert(r.message)}catch(e){alert(e.message)}}"
"async function loadLogs(ev){try{await busy('Actualizando logs...',async()=>{let l=await api('/api/logs');logBox.textContent=l.logs.map(x=>`${x.timeSec}s ${x.level} ${x.tag}: ${x.message}`).join('\\n')},ev&&ev.target)}catch(e){logBox.textContent=e.message}}"
"loadStatus();stateTimer=setInterval(loadStatus,3000);</script></body></html>";

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
    wifi_scan_config_t scan_config = {0};
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return err;
    }

    uint16_t count = WEB_WIFI_SCAN_MAX;
    wifi_ap_record_t records[WEB_WIFI_SCAN_MAX] = {0};
    esp_wifi_scan_get_ap_records(&count, records);

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
    esp_err_t err = telegram_bot_send_test_message(message);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return err;
    }
    return send_json(req, "{\"ok\":true,\"message\":\"Mensaje enviado\"}");
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
    config.max_uri_handlers = 14;
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

    ESP_LOGI(TAG, "Servidor web iniciado en puerto 80");
    return ESP_OK;
}
