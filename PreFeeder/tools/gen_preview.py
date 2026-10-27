import re
import pathlib

root = pathlib.Path(__file__).resolve().parents[1]
text = (root / "page.h").read_text(encoding="utf-8")
html = re.search(r'R"html\((.*)\)html"', text, re.S).group(1).strip()
banner = (
    '<p class="note" style="background:#1f3d2a;border:1px solid #238636;'
    'padding:10px;border-radius:8px;margin-bottom:12px">'
    "Vista previa local. Botones no llaman al ESP32.</p>"
)
html = html.replace('<header class="hdr">', banner + '<header class="hdr">', 1)
mock = (
    'var MOCK={side:"L",home:{raw:false,stable:false},'
    'sensors:{mid:true,high:false,max:false,tension:true,cylinder:false,hose:false,holgura:true},'
    "fault:0,idleMode:false,inProcess:true,benchInProcess:false,peer:{link:true},"
    "auto:{enabled:true,rpm:42,reverse_s:2.5,buffer_refill_fault_s:25,"
    "tension_reverse_rpm:30,tension_cooldown_s:1.5,tension_fault_s:8,"
    "servo_pwm_us:1500,"
    "mid_boost_rpm:55,mid_boost_s:1.2,mid_servo_delta_us:80,"
    "high_boost_rpm:70,high_boost_s:0.8,high_servo_delta_us:120},"
    "trigger2:{trigger_feed_s:1.5,m2_rpm:28,trigger_active:false,hmi_tfeed_enabled:true,"
    "holgura_settle_ms:150,holgura_on_s:2,holgura_off_fault_s:5,holgura_on:true},"
    'refill:{pulse_s:1},actuators:{feeder:false,buffer:true}};\n'
    "function pull(){sync(MOCK);}"
)
html = re.sub(r"function pull\(\)\{fetch\('/api/status',\{cache:'no-store'\}\).*?\.then\(function\(j\)\{sync\(j\);\}\)\}", mock, html)
preview_cfg = (
    "function cfg(u,btn){MOCK.save={ok:true};sync(MOCK,{forceForm:true});btnSaveFeedback(btn,true);}\n"
)
html = re.sub(r"function cfg\(u\)\{[\s\S]*?\};\nfunction pull", preview_cfg + "\nfunction pull", html)
html = re.sub(
    r"q\('saveRefill'\)\.onclick=function\(\)\{[\s\S]*?\};",
    "",
    html,
    count=1,
)
(root / "preview_ui.html").write_text(html, encoding="utf-8")
print("wrote preview_ui.html", len(html), "chars")
