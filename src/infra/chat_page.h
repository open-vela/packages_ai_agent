/* chat_page.h - 手机对话网页（自动生成，请勿手改）
 *
 * 源文件：packages/ai_agent/res/phone_chat.html
 * 生成：python3 tools/gen_chat_page.py
 *
 * 手机浏览器打开 http://<设备IP>:28789/ 时由 api_handler 直接返回本页，
 * 页面自身再连同一端口的 WebSocket，因此不需要任何 App 或外网。
 */

#ifndef AI_AGENT_CHAT_PAGE_H
#define AI_AGENT_CHAT_PAGE_H

static const char CHAT_PAGE_HTML[] =
    "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content="
    "\"width=device-width,initial-scale=1\">\n<title>\345\260\217\344\272\221\346\211\213\350\241\250 \302\267 \346\211\213\346\234\272\345\257\271\350\257\235</title>\n<style>\n*{box-"
    "sizing:border-box;-webkit-tap-highlight-color:transparent}\nbody{margin:0;height:100dvh;display:f"
    "lex;flex-direction:column;background:#12121f;color:#eef1f7;font:16px/1.5 -apple-system,\"PingFang"
    " SC\",sans-serif}\nheader{display:flex;align-items:center;justify-content:space-between;gap:8px;pa"
    "dding:12px 16px 8px}\nh1{margin:0;font-size:18px;font-weight:600}\nh1 span{color:#9aa3b8;font-size"
    ":13px;font-weight:400;margin-left:6px}\n#status{font-size:12px;padding:4px 10px;border-radius:999"
    "px;border:1px solid #33334d;color:#9aa3b8;white-space:nowrap}\n#status.on{color:#3ddc84;border-co"
    "lor:rgba(61,220,132,.5)}\n#status.bad{color:#ff7676;border-color:rgba(255,118,118,.5)}\nmain{flex:"
    "1;overflow-y:auto;padding:4px 16px 10px}\n.row{display:flex;margin:10px 0}\n.row.me{justify-conten"
    "t:flex-end}\n.b{max-width:82%;padding:10px 14px;border-radius:16px;background:#262640;border:1px "
    "solid #33334d;white-space:pre-wrap;word-break:break-word}\n.row.me .b{background:#2f6fd0;border-c"
    "olor:transparent}\n.m{font-size:11px;color:#9aa3b8;margin:2px 6px 0}\n.row.me+.m{text-align:right}"
    "\n#tip{color:#9aa3b8;font-size:14px;text-align:center;margin-top:26vh}\n#chips{display:flex;gap:8p"
    "x;overflow-x:auto;padding:8px 16px}\n#chips button{flex:0 0 auto;font:inherit;font-size:13px;colo"
    "r:#eef1f7;background:#1a1a2e;border:1px solid #33334d;border-radius:999px;padding:8px 14px}\nfoot"
    "er{display:flex;gap:8px;padding:8px 16px 14px;border-top:1px solid #33334d;background:#1a1a2e}\n#"
    "txt{flex:1;font:inherit;color:#eef1f7;background:#12121f;border:1px solid #33334d;border-radius:"
    "12px;padding:12px 14px}\n#txt:focus{outline:none;border-color:#2f6fd0}\n#send{font:inherit;font-we"
    "ight:600;color:#fff;background:#2f6fd0;border:0;border-radius:12px;padding:12px 20px}\n</style>\n<"
    "/head>\n<body>\n<header>\n<h1>\345\260\217\344\272\221\346\211\213\350\241\250<span>\302\267 \346\211\213\346\234\272\345\257\271\350\257\235</span></h1>\n<div id=\"status\">\350\277\236\346\216\245"
    "\344\270\255\342\200\246</div>\n</header>\n<main id=\"log\"><div id=\"tip\">\346\255\243\345\234\250\351\200\232\350\277\207\350\223\235\347\211\231\347\275\221\347\273\234\350\277\236\346\216\245\346\211\213\350\241\250\342\200\246</div"
    "></main>\n<div id=\"chips\">\n<button data-q=\"3\345\210\206\351\222\237\345\220\216\346\217\220\351\206\222\346\210\221\345\226\235\346\260\264\">\342\217\260 3\345\210\206\351\222\237\345\220\216\346\217\220\351\206\222\346\210\221\345\226\235\346"
    "\260\264</button>\n<button data-q=\"\345\217\226\346\266\210\346\217\220\351\206\222\">\342\217\260 \345\217\226\346\266\210\346\217\220\351\206\222</button>\n<button data-q=\"\347\216\260\345\234\250\345\207\240\347\202\271"
    "\344\272\206\">\360\237\225\220 \347\216\260\345\234\250\345\207\240\347\202\271\344\272\206</button>\n<button data-q=\"\346\211\223\345\274\200\345\256\242\345\216\205\347\232\204\347\201\257\">\360\237\222\241 \346\211\223\345\274\200\345\256\242\345\216\205\347\232\204\347\201\257</"
    "button>\n<button data-q=\"\345\214\227\344\272\254\344\273\212\345\244\251\345\244\251\346\260\224\346\200\216\344\271\210\346\240\267\">\360\237\214\244 \345\214\227\344\272\254\344\273\212\345\244\251\345\244\251\346\260\224\346\200\216\344\271\210\346\240\267</button>\n<"
    "/div>\n<footer>\n<input id=\"txt\" placeholder=\"\345\257\271\345\245\271\350\257\264\347\202\271\344\273\200\344\271\210\342\200\246\" autocomplete=\"off\" enterkeyhi"
    "nt=\"send\">\n<button id=\"send\">\345\217\221\351\200\201</button>\n</footer>\n<script>\n/* \351\241\265\351\235\242\347\224\261\346\211\213\350\241\250\346\211\230\347\256\241\357\274\210\345\220\214\344\270"
    "\273\346\234\272\345\220\214\347\253\257\345\217\243 28789\357\274\211\357\274\214\351\241\265\351\235\242\345\206\215\350\277\236\345\220\214\344\270\200\347\253\257\345\217\243\347\232\204 WebSocket\343\200\202\n   \350\242\253\345\255\230\346\210\220\346\234\254\345\234\260\346\226\207\344\273\266\346\211\223\345\274\200"
    "\346\227\266 location.host \344\270\272\347\251\272\357\274\214\351\200\200\345\233\236\351\273\230\350\256\244\345\234\260\345\235\200\343\200\202 */\n(function(){\n'use strict';\nvar host=location"
    ".host||'192.168.44.140:28789',url='ws://'+host+'/';\nvar st=document.getElementById('status'),log"
    "=document.getElementById('log'),\ntip=document.getElementById('tip'),txt=document.getElementById("
    "'txt'),\nws=null,retry=0;\n\nfunction status(t,c){st.textContent=t;st.className=c||'';}\n/* \345\277\203\350\267\263\357\274"
    "\232\350\204\232\346\234\254\350\267\221\345\210\260\350\277\231\351\207\214\345\260\261\344\274\232\350\257\267\346\261\202 /alive\343\200\202\350\256\276\345\244\207\346\227\245\345\277\227\351\207\214\350\203\275\347\234\213\345\210\260\350\277\231\350\241\214\357\274\214\345\260\261\347\255\211\344\272\216\350\257\201\346\230\216\n   \"\351\241"
    "\265\351\235\242\350\242\253\345\256\214\346\225\264\351\200\201\350\276\276\344\270\224\350\204\232\346\234\254\346\211\247\350\241\214\344\272\206\"\342\200\224\342\200\224\345\220\246\345\210\231\350\257\264\346\230\216\344\274\240\350\276\223\344\270\255\346\226\255\345\234\250\346\234\253\345\260\276\343\200\202 */\ntry{var beac"
    "on=new Image();beacon.src='/alive?t='+Date.now();}catch(e){}\nif(tip)tip.textContent='\350\204\232\346\234\254\345\267\262\350\277"
    "\220\350\241\214 \302\267 \346\255\243\345\234\250\350\277\236\346\216\245\346\211\213\350\241\250\342\200\246';\nfunction now(){var d=new Date();return ('0'+d.getHours()).slice("
    "-2)+':'+('0'+d.getMinutes()).slice(-2);}\nfunction add(text,me,src){\n  if(tip){tip.remove();tip=n"
    "ull;}\n  var r=document.createElement('div');r.className='row'+(me?' me':'');\n  var b=document.cr"
    "eateElement('div');b.className='b';b.textContent=text;\n  r.appendChild(b);log.appendChild(r);\n  "
    "var m=document.createElement('div');m.className='m';\n  /* \346\240\207\346\263\250\350\277\231\346\254\241\346\230\257\350\260\201\347\255\224\347\232\204\357\274\232\347\253\257\344\276\247\357\274\210\346\234"
    "\254\345\234\260\357\274\211\350\277\230\346\230\257\344\272\221\347\253\257 \342\200\224\342\200\224 \347\253\257\344\272\221\345\215\217\345\220\214\345\234\250\347\225\214\351\235\242\344\270\212\345\217\257\350\247\201 */\n  m.textContent=(me?'\346\210\221':'\345\260\217\344\272\221')"
    "+' \302\267 '+now()+(src?(src==='local'?' \302\267 \346\234\254\345\234\260':' \302\267 \344\272\221\347\253\257'):'');\n  log.appendChild(m);\n  log.sc"
    "rollTop=log.scrollHeight;\n}\nfunction connect(){\n  if(ws&&(ws.readyState===0||ws.readyState===1))"
    "return;\n  status('\350\277\236\346\216\245\344\270\255\342\200\246','');\n  try{ws=new WebSocket(url);}\n  catch(e){status('\346\227\240\346\263\225\350\277\236\346"
    "\216\245','bad');retry++;setTimeout(connect,2000);return;}\n  ws.onopen=function(){\n    retry=0;status("
    "'\345\267\262\350\277\236\346\216\245 \302\267 \350\223\235\347\211\231\347\275\221\347\273\234','on');\n    /* \350\256\244\351\242\206\350\272\253\344\273\275\357\274\232\346\211\213\350\241\250\344\274\232\346\212\212\346\226\255\347\272\277\346\234\237\351\227\264\346\232\202\345\255\230\347\232\204\345\233\236\345\244\215"
    "\347\253\213\345\210\273\350\241\245\345\217\221\347\273\231\346\210\221 */\n    try{ws.send(JSON.stringify({type:'hello',chat_id:'phone'}));}catch(e){"
    "}\n  };\n  ws.onmessage=function(ev){\n    var d=ev.data;\n    try{var m=JSON.parse(d);if(m&&m.type="
    "=='response'&&m.content)return add(m.content,false,m.source);}catch(e){}\n    if(d)add(String(d),"
    "false);\n  };\n  ws.onclose=function(){status('\345\267\262\346\226\255\345\274\200 \302\267 \350\207\252\345\212\250\351\207\215\350\277\236','bad');retry++;setTimeou"
    "t(connect,Math.min(1500+retry*500,6000));};\n  ws.onerror=function(){};\n}\nfunction send(t){\n  t=("
    "t||'').trim();if(!t)return;\n  add(t,true);\n  if(ws&&ws.readyState===1){\n    ws.send(JSON.stringi"
    "fy({type:'message',content:t,chat_id:'phone'}));\n    /* \"\346\255\243\345\234\250\346\200\235\350\200\203\342\200\246\" \347\224\261\346\211\213\350\241\250\350\207\252\345\267\261\345\217\221\357\274\210a"
    "gent \347\232\204 working \347\212\266\346\200\201\350\257\255\357\274\211\357\274\214\344\270\215\345\234\250\350\277\231\351\207\214\351\207\215\345\244\215 */\n  }else{\n    add('\357\274\210\350\277\230\346\262\241\350\277\236\344\270\212\346\211\213\350\241\250\357\274\214"
    "\346\255\243\345\234\250\351\207\215\350\257\225\342\200\246\357\274\211',false);\n  }\n}\ndocument.getElementById('send').onclick=function(){send(txt.va"
    "lue);txt.value='';txt.focus();};\ntxt.onkeydown=function(e){if(e.key==='Enter'){send(txt.value);t"
    "xt.value='';}};\ndocument.getElementById('chips').onclick=function(e){\n  var q=e.target&&e.target"
    ".getAttribute&&e.target.getAttribute('data-q');\n  if(q)send(q);\n};\nconnect();\n})();\n</script>\n</"
    "body>\n</html>\n"
;

#endif /* AI_AGENT_CHAT_PAGE_H */
