#pragma once
#include <pgmspace.h>

// /view page (LIVE). Uses the same NAV style as other pages.
// The current page is highlighted via class="active".
static const char index_html[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>BirdCam</title>
<style>
:root{--amber:#ffb000;--amber2:#ffcc55;--bg:#060606;--panel:#0b0b0b;}
body{margin:0;background:var(--bg);color:var(--amber);font-family:ui-monospace,Consolas,monospace;}
a{color:inherit;text-decoration:none;}
.nav{background:var(--amber);color:#000;padding:10px 14px;font-weight:bold;letter-spacing:.5px;}
.nav a{
  margin-right:14px;color:#000;
  font-variant: small-caps;
  text-transform: uppercase;
  letter-spacing: .6px;
  padding-bottom:2px;
}
.nav a.active{border-bottom:3px solid rgba(0,0,0,.75);}
.wrap{max-width:1100px;margin:0 auto;padding:14px;}
.card{background:rgba(10,10,10,.75);border-radius:14px;padding:14px;box-shadow:0 0 24px rgba(0,0,0,.5);}
.media{position:relative;display:inline-block;border-radius:16px;overflow:hidden;}
.media::after{content:'';position:absolute;inset:0;
  background:linear-gradient(to bottom,rgba(0,0,0,.12) 50%,rgba(0,0,0,.32) 50%);
  background-size:100% 3px;mix-blend-mode:multiply;pointer-events:none;opacity:.55;}
.media::before{content:'';position:absolute;inset:-40px;
  background:radial-gradient(circle at 50% 50%, rgba(255,176,0,.22), transparent 60%);
  pointer-events:none;filter:blur(10px);opacity:.7;}
.frame{border:2px solid rgba(255,176,0,.75);border-radius:16px;box-shadow:0 0 36px rgba(255,176,0,.2);}
img{display:block;max-width:100%;height:auto;}
</style>
</head>

<body>
  <div class='nav'>
    <a class='active' href='/view'>LIVE</a>
    <a href='/status'>STATUS</a>
    <a href='/archive'>GALLERY</a>
    <a href='/settings'>SETTINGS</a>
  </div>

  <div class='wrap'>
    <div class='card'>
      <div class='media frame' style='background:#000'>
        <img id='m' src='/mjpeg'>
      </div>
    </div>
  </div>

<script>
(async () => {
  try {
    const r = await fetch('/api/mode', {cache:'no-store'});
    const mode = parseInt(await r.text(), 10) || 0;
    const img = document.getElementById('m');

    // mode 4/5: rotazione SOLO web
    if (mode === 4) img.style.transform = 'rotate(90deg)';
    if (mode === 5) img.style.transform = 'rotate(-90deg)';
  } catch(e) {}
})();
</script>
</body>
</html>
)rawliteral";
