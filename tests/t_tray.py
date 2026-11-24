#!/usr/bin/python3 -I
"""Isolated test of the MaxEco tray.

Run inside a private bus:  dbus-run-session -- python3 -I tests/t_tray.py build/maxeco
It plays the tray host (a fake org.kde.StatusNotifierWatcher), a fake
notification server and a fake power-profiles-daemon (which must never be
called), and replaces systemctl by tests/fake-systemctl and kscreen-doctor by
tests/fake-kscreen-doctor. Nothing outside the temporary directory is touched:
the real screens are never changed.
"""
import os
import signal
import subprocess
import sys
import tempfile
import time

from gi.repository import Gio, GLib

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/maxeco")
HERE = os.path.dirname(os.path.abspath(__file__))
TMP = tempfile.mkdtemp(prefix="maxeco-test-")
CFG = os.path.join(TMP, "config")
STATE = os.path.join(TMP, "run-state")
LEVEL = os.path.join(TMP, "run-level")
LOG = os.path.join(TMP, "systemctl.log")
KLOG = os.path.join(TMP, "kscreen.log")
SCREENS = os.path.join(TMP, "screens")
DISPLAY_FILE = os.path.join(CFG, "maxeco/display")
HZ_FILE = os.path.join(CFG, "maxeco/screens-60hz")
SAVED = os.path.join(CFG, "maxeco/state")
RESTORE_ENTRY = os.path.join(CFG, "autostart/maxeco-restore-display.desktop")
ECO_ARGS = "output.HDMI-A-2.mode.1 output.DP-4.mode.29"
BACK_ARGS = "output.HDMI-A-2.mode.2 output.DP-4.mode.26"
ORIGINAL = "HDMI-A-2 2\nDP-4 26\nHDMI-A-1 40\n"
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
fails = 0


def check(name, got, want):
    global fails
    ok = got == want
    fails += not ok
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": got {got!r} want {want!r}"))


def spin(cond, timeout=5.0):
    ctx = GLib.MainContext.default()
    end = time.time() + timeout
    while time.time() < end:
        while ctx.pending():
            ctx.iteration(False)
        if cond():
            return True
        time.sleep(0.01)
    return False


# ---------------------------------------------------------------- fakes
registered = []
notifications = []
holds = []          # (sender, profile, reason, app)
released = []


def own(name, xml, handler):
    node = Gio.DBusNodeInfo.new_for_xml(xml)
    path = "/" + name.split(".")[-1] if name != "org.kde.StatusNotifierWatcher" else "/StatusNotifierWatcher"
    return path, node


WATCHER_XML = """<node><interface name="org.kde.StatusNotifierWatcher">
<method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
</interface></node>"""
NOTIFY_XML = """<node><interface name="org.freedesktop.Notifications">
<method name="Notify"><arg type="s" direction="in"/><arg type="u" direction="in"/>
<arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/>
<arg type="as" direction="in"/><arg type="a{sv}" direction="in"/><arg type="i" direction="in"/>
<arg type="u" direction="out"/></method></interface></node>"""
PPD_XML = """<node><interface name="org.freedesktop.UPower.PowerProfiles">
<method name="HoldProfile"><arg type="s" direction="in"/><arg type="s" direction="in"/>
<arg type="s" direction="in"/><arg type="u" direction="out"/></method></interface></node>"""


def on_call(conn, sender, path, iface, method, params, inv):
    if method == "RegisterStatusNotifierItem":
        registered.append(params.unpack()[0])
        inv.return_value(None)
    elif method == "Notify":
        notifications.append(params.unpack()[3:5])
        inv.return_value(GLib.Variant("(u)", (1,)))
    elif method == "HoldProfile":
        holds.append((sender,) + tuple(params.unpack()))
        inv.return_value(GLib.Variant("(u)", (len(holds),)))


reg_ids = {}


def export(path, xml):
    node = Gio.DBusNodeInfo.new_for_xml(xml)
    reg_ids[path] = bus.register_object(path, node.interfaces[0], on_call, None, None)


export("/StatusNotifierWatcher", WATCHER_XML)
export("/org/freedesktop/Notifications", NOTIFY_XML)
export("/org/freedesktop/UPower/PowerProfiles", PPD_XML)


def request(name):
    r = bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                      "RequestName", GLib.Variant("(su)", (name, 4)), None, 0, -1, None)
    assert r.unpack()[0] == 1, name


def release(name):
    bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                  "ReleaseName", GLib.Variant("(s)", (name,)), None, 0, -1, None)


for n in ("org.kde.StatusNotifierWatcher", "org.freedesktop.Notifications",
          "org.freedesktop.UPower.PowerProfiles"):
    request(n)


def on_noc(conn, sender, path, iface, sig, params):
    name, old, new = params.unpack()
    if not new and any(h[0] == name for h in holds):
        released.append(name)


bus.signal_subscribe("org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                     "/org/freedesktop/DBus", None, 0, on_noc)
signals = []
bus.signal_subscribe(None, None, None, None, None, 0,
                     lambda c, s, p, i, m, a: signals.append(m) if i in (
                         "org.kde.StatusNotifierItem", "com.canonical.dbusmenu") else None)

# ------------------------------------------------------------ the tray
env = dict(os.environ)
env.update(XDG_CONFIG_HOME=CFG, XDG_RUNTIME_DIR=TMP, MAXECO_STATE=STATE, MAXECO_LEVEL=LEVEL,
           MAXECO_SYSTEMCTL=os.path.join(HERE, "fake-systemctl"), FAKE_LOG=LOG,
           MAXECO_KSCREEN=os.path.join(HERE, "fake-kscreen-doctor"), FAKE_KLOG=KLOG,
           FAKE_SCREENS=SCREENS, DBUS_SYSTEM_BUS_ADDRESS=os.environ["DBUS_SESSION_BUS_ADDRESS"])
for k in ("HOME", "WAYLAND_DISPLAY", "DISPLAY"):
    env.pop(k, None)


def klog():
    return open(KLOG).read().splitlines() if os.path.exists(KLOG) else []


def screens():
    return open(SCREENS).read() if os.path.exists(SCREENS) else ORIGINAL
proc = None


def launch(extra=None):
    global proc
    e = dict(env)
    e.update(extra or {})
    proc = subprocess.Popen([BIN], env=e, stderr=subprocess.PIPE, text=True)
    return proc


def item():
    return registered[-1]


def call(path, iface, method, sig=None, args=None, reply=None, timeout=3000):
    return bus.call_sync(item(), path, iface, method, GLib.Variant(sig, args) if sig else None,
                         GLib.VariantType(reply) if reply else None, 0, timeout, None).unpack()


def prop(name):
    return call("/StatusNotifierItem", "org.freedesktop.DBus.Properties", "Get", "(ss)",
                ("org.kde.StatusNotifierItem", name))[0]


def layout():
    rev, (rid, rprops, children) = call("/MenuBar", "com.canonical.dbusmenu", "GetLayout", "(iias)",
                                        (0, -1, []))
    return rev, rprops, children


def click(i):
    call("/MenuBar", "com.canonical.dbusmenu", "Event", "(isvu)",
         (i, "clicked", GLib.Variant("i", 0), 0))


def labels():
    return [c[1].get("label", c[1].get("type")) for c in layout()[2]]


def states():
    return {c[1]["label"]: c[1].get("toggle-state") for c in layout()[2] if "label" in c[1]}



NORMAL, ECO, MAX, HZ, AUTO, QUIT = 1, 2, 3, 5, 6, 8
ICON = {NORMAL: "maxeco-off", ECO: "maxeco-eco", MAX: "maxeco-on"}


def syslog():
    return open(LOG).read().splitlines() if os.path.exists(LOG) else []


def saved():
    return open(SAVED).read() if os.path.exists(SAVED) else None


def scroll(delta, direction="vertical"):
    call("/StatusNotifierItem", "org.kde.StatusNotifierItem", "Scroll", "(is)", (delta, direction))


def settled(lv):
    """the tray reached level lv and has nothing running"""
    return spin(lambda: prop("IconName") == ICON[lv] and not prop("ToolTip")[3].startswith("Switching"))


def level_now():
    if not os.path.exists(STATE):
        return "normal"
    return open(LEVEL).read().strip() if os.path.exists(LEVEL) else "max"


launch()
check("registers with the watcher", spin(lambda: registered), True)
name = item()
check("item name", name, f"org.kde.StatusNotifierItem-{proc.pid}-1")

props = call("/StatusNotifierItem", "org.freedesktop.DBus.Properties", "GetAll", "(s)",
             ("org.kde.StatusNotifierItem",))[0]
check("IconName Normal", props["IconName"], "maxeco-off")
check("Menu path", props["Menu"], "/MenuBar")
check("ItemIsMenu", props["ItemIsMenu"], True)
check("Category", props["Category"], "Hardware")
check("ToolTip title", props["ToolTip"][2], "MaxEco")
check("ToolTip Normal", props["ToolTip"][3].split("\n")[0], "Normal. Everything runs as usual.")
check("all SNI properties", sorted(props), sorted(
    ["Category", "Id", "Title", "Status", "WindowId", "IconThemePath", "Menu", "ItemIsMenu",
     "IconName", "IconPixmap", "OverlayIconName", "OverlayIconPixmap", "AttentionIconName",
     "AttentionIconPixmap", "AttentionMovieName", "ToolTip"]))
for p in ("/", "/StatusNotifierItem", "/MenuBar"):
    xml = call(p, "org.freedesktop.DBus.Introspectable", "Introspect", reply="(s)")[0]
    Gio.DBusNodeInfo.new_for_xml(xml)
    print("ok   introspection XML parses for", p)
mprops = call("/MenuBar", "org.freedesktop.DBus.Properties", "GetAll", "(s)", ("com.canonical.dbusmenu",))[0]
check("dbusmenu Version", mprops["Version"], 3)
check("menu entries", labels(), ["Normal", "Eco", "MaxEco", "separator", "60 Hz Screens in MaxEco",
                                 "Start With System", "separator", "Quit"])
check("radio and checkmarks at start", states(), {"Normal": 1, "Eco": 0, "MaxEco": 0, "60 Hz Screens in MaxEco": 0,
                                                 "Start With System": 0, "Quit": None})
check("radio group", [c[1].get("toggle-type") for c in layout()[2][:3]], ["radio"] * 3)
gp = call("/MenuBar", "com.canonical.dbusmenu", "GetGroupProperties", "(aias)", ([1, 8], []))[0]
check("GetGroupProperties", [(i, p["label"]) for i, p in gp], [(1, "Normal"), (8, "Quit")])
check("GetGroupProperties all", len(call("/MenuBar", "com.canonical.dbusmenu", "GetGroupProperties",
                                         "(aias)", ([], []))[0]), 9)
check("GetProperty", call("/MenuBar", "com.canonical.dbusmenu", "GetProperty", "(is)", (6, "label"))[0],
      "Start With System")
check("AboutToShow", call("/MenuBar", "com.canonical.dbusmenu", "AboutToShow", "(i)", (0,))[0], False)
check("AboutToShowGroup", call("/MenuBar", "com.canonical.dbusmenu", "AboutToShowGroup", "(ai)", ([0],)),
      ([], []))
call("/StatusNotifierItem", "org.kde.StatusNotifierItem", "Activate", "(ii)", (1, 2))
scroll(0)
scroll(120)  # wheel up at Normal: already the top
scroll(-120, "horizontal")  # sideways is ignored
call("/", "org.freedesktop.DBus.Peer", "Ping")
time.sleep(0.3)
check("Activate, Scroll up at Normal and sideways do nothing", (syslog(), prop("IconName")), ([], "maxeco-off"))

# ----------------------------------------------------- errors and abuse
for args, err in [(("/MenuBar", "com.canonical.dbusmenu", "Nope"), "UnknownMethod"),
                  (("/MenuBar", "com.canonical.dbusmenu", "GetLayout", "(s)", ("x",)), "UnknownMethod"),
                  (("/MenuBar", "com.canonical.dbusmenu", "GetLayout", "(iias)", (99, -1, [])), "InvalidArgs"),
                  (("/nothing", "org.kde.StatusNotifierItem", "Activate", "(ii)", (0, 0)), "UnknownObject"),
                  (("/StatusNotifierItem", "org.freedesktop.DBus.Properties", "Get", "(ss)",
                    ("org.kde.StatusNotifierItem", "Nope")), "UnknownProperty"),
                  (("/StatusNotifierItem", "org.freedesktop.DBus.Properties", "Set", "(ssv)",
                    ("org.kde.StatusNotifierItem", "Id", GLib.Variant("s", "x"))), "PropertyReadOnly")]:
    try:
        call(*args)
        check("error " + err, "no error", err)
    except GLib.Error as e:
        check("error " + err, err in e.message, True)
try:  # a message bigger than the tray's buffer is dropped without a reply
    call("/StatusNotifierItem", "org.freedesktop.DBus.Properties", "Get", "(ss)",
         ("x" * 100000, "y"), timeout=500)
    check("oversized message dropped", "replied", "dropped")
except GLib.Error as e:
    check("oversized message dropped", "Timeout" in e.message or "timed out" in e.message.lower(), True)
deep = GLib.Variant("v", GLib.Variant("i", 1))
for _ in range(60):
    deep = GLib.Variant("v", deep)
try:
    call("/MenuBar", "com.canonical.dbusmenu", "Event", "(isvu)", (8, "hovered", deep, 0), timeout=1000)
except GLib.Error:
    pass
try:
    call("/StatusNotifierItem", "org.kde.StatusNotifierItem", "Scroll", "(ss)", ("x", "y"), timeout=1000)
except GLib.Error:
    pass
check("still alive after abuse", (prop("Id"), syslog()), ("maxeco", []))

# ------------------------------------------------------------- the levels
signals.clear()
click(ECO)
check("Eco starts maxeco-level@eco", settled(ECO) and level_now() == "eco", True)
check("systemctl call", syslog(), ["start maxeco-level@eco.service"])
check("radio on Eco", states()["Eco"], 1)
check("ToolTip Eco", prop("ToolTip")[3].split("\n")[0],
      "Eco. CPU and GPU use less power and games still run, quiet fans.")
check("signals sent", {"LayoutUpdated", "NewIcon", "NewToolTip"} <= set(signals), True)
check("saved level eco", saved(), "eco\n")
check("power-profiles-daemon is left alone", holds, [])
click(ECO)
time.sleep(0.3)
check("the active level again does nothing", len(syslog()), 1)
click(MAX)
check("MaxEco starts maxeco-level@max", settled(MAX) and level_now() == "max", True)
check("systemctl call", syslog()[-1], "start maxeco-level@max.service")
check("saved level max", saved(), "max\n")
check("ToolTip MaxEco", prop("ToolTip")[3].split("\n")[0], "MaxEco. CPU and GPU at their lowest power, quiet fans.")
check("no screen change without the 60 Hz option", (klog(), screens()), ([], ORIGINAL))

click(HZ)
check("60 Hz option puts the fast screens at 60 Hz (disabled one untouched)", spin(lambda: klog() == [ECO_ARGS]), True)
check("screens now", screens(), "HDMI-A-2 1\nDP-4 29\nHDMI-A-1 40\n")
check("option saved", (states()["60 Hz Screens in MaxEco"], os.path.exists(HZ_FILE)), (1, True))
check("previous modes saved", open(DISPLAY_FILE).read(), "HDMI-A-2 1920 1080 10000\nDP-4 1920 1080 24000\n")
check("ToolTip MaxEco 60 Hz", prop("ToolTip")[3].split("\n")[0],
      "MaxEco. CPU and GPU at their lowest power, quiet fans, screens at 60 Hz.")
check("login restore entry while not starting with the system", os.path.exists(RESTORE_ENTRY), True)
check("restore entry Exec", [l for l in open(RESTORE_ENTRY) if l.startswith("Exec=")],
      [f"Exec={BIN} --restore-display\n"])

click(AUTO)
auto = os.path.join(CFG, "autostart/maxeco.desktop")
check("Start With System creates the autostart entry", spin(lambda: os.path.exists(auto)), True)
check("autostart Exec", [l for l in open(auto) if l.startswith("Exec=")], [f"Exec={BIN}\n"])
check("checkmark on", states()["Start With System"], 1)
check("no restore entry when the tray itself starts with the system", os.path.exists(RESTORE_ENTRY), False)
click(AUTO)
check("unchecking removes it", spin(lambda: not os.path.exists(auto)), True)
check("restore entry back", os.path.exists(RESTORE_ENTRY), True)
os.makedirs(os.path.dirname(auto), exist_ok=True)
open(auto, "w").write("[Desktop Entry]\nExec=x\nHidden=true\n")
call("/MenuBar", "com.canonical.dbusmenu", "AboutToShow", "(i)", (0,))
check("Hidden=true counts as off", states()["Start With System"], 0)
os.remove(auto)

click(ECO)
check("MaxEco to Eco", settled(ECO) and level_now() == "eco", True)
check("leaving MaxEco puts the screens back", (klog()[-1:], screens(), os.path.exists(DISPLAY_FILE)),
      ([BACK_ARGS], ORIGINAL, False))
check("restore entry removed", os.path.exists(RESTORE_ENTRY), False)
click(MAX)
check("back to MaxEco goes to 60 Hz again", settled(MAX) and spin(lambda: screens() != ORIGINAL), True)
click(HZ)
check("unchecking the option puts the screens back", spin(lambda: screens() == ORIGINAL), True)
check("option removed", (states()["60 Hz Screens in MaxEco"], os.path.exists(HZ_FILE)), (0, False))
check("still MaxEco", level_now(), "max")
n = len(klog())
click(HZ)
check("checking it again in MaxEco", spin(lambda: screens() != ORIGINAL), True)

click(NORMAL)
check("Normal stops maxeco.service", settled(NORMAL) and level_now() == "normal", True)
check("systemctl call", syslog()[-1], "stop maxeco.service")
check("saved level normal", saved(), "normal\n")
check("Normal puts the screens back", (klog()[-1:], screens(), os.path.exists(DISPLAY_FILE)),
      ([BACK_ARGS], ORIGINAL, False))

# ----------------------------------------------------------- mouse wheel
open(LOG, "w").close()
scroll(-120)
check("wheel down: Eco", settled(ECO) and level_now() == "eco", True)
scroll(-60)
time.sleep(0.3)
check("half a notch does nothing yet", level_now(), "eco")
scroll(-60)
check("the other half: MaxEco", settled(MAX) and level_now() == "max", True)
check("MaxEco by the wheel also follows the 60 Hz option", spin(lambda: screens() != ORIGINAL), True)
scroll(-120)
time.sleep(0.3)
check("wheel down at MaxEco: already the bottom", len(syslog()), 2)
scroll(240)
check("two notches up: Normal in one step", settled(NORMAL) and level_now() == "normal", True)
check("systemctl calls", syslog(), ["start maxeco-level@eco.service", "start maxeco-level@max.service",
                                    "stop maxeco.service"])
check("screens back", screens(), ORIGINAL)
open(LOG, "w").close()
scroll(-120)
time.sleep(0.05)
scroll(-120)  # while Eco is still being applied
check("fast wheel: the last pick wins", settled(MAX) and level_now() == "max", True)
check("systemctl calls", syslog(), ["start maxeco-level@eco.service", "start maxeco-level@max.service"])
scroll(120)
time.sleep(0.05)
scroll(120)
time.sleep(0.05)
scroll(-120)
check("up, up, down ends on Eco", settled(ECO) and level_now() == "eco", True)
check("screens back after leaving MaxEco", spin(lambda: screens() == ORIGINAL), True)
click(NORMAL)
settled(NORMAL)

# --------------------------------------------------- failure and Quit
proc.send_signal(signal.SIGTERM)
check("SIGTERM exits cleanly", proc.wait(5), 0)
launch({"FAKE_RC": "1"})
spin(lambda: registered and registered[-1] != name)
name = item()
k = len(klog())
click(MAX)
check("failed switch shows a notification", spin(lambda: notifications), True)
check("notification text", notifications[-1][0], "MaxEco could not switch to MaxEco")
check("icon and radio back on Normal", spin(lambda: prop("IconName") == "maxeco-off") and states()["Normal"], 1)
check("failed switch leaves the screens alone", (len(klog()), screens()), (k, ORIGINAL))
check("saved level stays normal", saved(), "normal\n")
proc.send_signal(signal.SIGTERM)
proc.wait(5)

launch()
spin(lambda: registered[-1] != name)
name = item()
second = subprocess.run([BIN], env=env, capture_output=True, text=True, timeout=5)
check("second instance refuses to run", (second.returncode, second.stderr.strip()), (0, "maxeco: already running"))
click(MAX)
settled(MAX)
check("screens at 60 Hz", spin(lambda: screens() != ORIGINAL), True)
open(LOG, "w").close()
click(QUIT)
check("Quit in MaxEco stops the service and exits", proc.wait(5), 0)
check("Quit ran systemctl stop", syslog(), ["stop maxeco.service"])
check("Quit leaves the system as before", (os.path.exists(STATE), os.path.exists(LEVEL)), (False, False))
check("Quit puts the screens back", (screens(), os.path.exists(DISPLAY_FILE), os.path.exists(RESTORE_ENTRY)),
      (ORIGINAL, False, False))
check("Quit saves normal", saved(), "normal\n")

launch()
spin(lambda: registered[-1] != name)
name = item()
open(LOG, "w").close()
click(ECO)
click(QUIT)  # while Eco is still being applied
check("Quit during a switch waits, then goes back to Normal and exits", proc.wait(5), 0)
check("systemctl calls", syslog(), ["start maxeco-level@eco.service", "stop maxeco.service"])
check("nothing left on", level_now(), "normal")

# ------------------- PC turned off in MaxEco (screens stayed at 60)
def turned_off_at_60():
    subprocess.run([os.path.join(HERE, "fake-kscreen-doctor"), *ECO_ARGS.split()], env=env, check=True)
    os.makedirs(os.path.dirname(RESTORE_ENTRY), exist_ok=True)
    open(DISPLAY_FILE, "w").write("HDMI-A-2 1920 1080 10000\nDP-4 1920 1080 24000\n")
    open(RESTORE_ENTRY, "w").write("[Desktop Entry]\n")


turned_off_at_60()
open(SAVED, "w").write("max\n")
r = subprocess.run([BIN, "--restore-display"], env=env, capture_output=True, text=True, timeout=10)
check("--restore-display at login", (r.returncode, screens(), os.path.exists(DISPLAY_FILE),
                                     os.path.exists(RESTORE_ENTRY)), (0, ORIGINAL, False, False))
turned_off_at_60()
r = subprocess.run([BIN, "--restore-display"], env=dict(env, FAKE_KRC="1"), capture_output=True, text=True,
                   timeout=10)
check("a failed restore keeps the saved modes", (r.returncode, os.path.exists(DISPLAY_FILE)), (1, True))
auto = os.path.join(CFG, "autostart/maxeco.desktop")
open(auto, "w").write("[Desktop Entry]\nExec=x\n")
r = subprocess.run([BIN, "--restore-display"], env=env, capture_output=True, text=True, timeout=10)
check("--restore-display leaves it to the tray when it starts with the system",
      (r.returncode, os.path.exists(DISPLAY_FILE)), (0, True))
os.remove(auto)
open(SAVED, "w").write("normal\n")
launch()
check("tray start puts the screens back", spin(lambda: screens() == ORIGINAL and not os.path.exists(DISPLAY_FILE)),
      True)
spin(lambda: registered[-1] != name)
name = item()
proc.send_signal(signal.SIGTERM)
proc.wait(5)

# ----------------------------------------- the saved level after a reboot
for word, lv, unit in (("eco", ECO, "eco"), ("max", MAX, "max"), ("on", MAX, "max")):
    open(SAVED, "w").write(word + "\n")
    open(LOG, "w").close()
    launch()
    spin(lambda: registered[-1] != name)
    name = item()
    check(f"saved {word!r} is applied again at start", settled(lv) and syslog() == [f"start maxeco-level@{unit}.service"],
          True)
    if word != "on":
        proc.send_signal(signal.SIGTERM)
        proc.wait(5)
        os.remove(STATE)
check("and with the 60 Hz option the screens go to 60 Hz again", spin(lambda: screens() != ORIGINAL), True)
# level changed behind the tray's back
open(LEVEL, "w").write("eco\n")
call("/MenuBar", "com.canonical.dbusmenu", "AboutToShow", "(i)", (0,))
check("outside change to Eco picked up when the menu opens", (prop("IconName"), states()["Eco"]), ("maxeco-eco", 1))
os.remove(STATE)
call("/MenuBar", "com.canonical.dbusmenu", "AboutToShow", "(i)", (0,))
check("outside stop picked up", (prop("IconName"), states()["Normal"]), ("maxeco-off", 1))
check("saved screen modes kept for later", open(DISPLAY_FILE).read(),
      "HDMI-A-2 1920 1080 10000\nDP-4 1920 1080 24000\n")

# watcher restart (plasmashell restart)
count = len(registered)
release("org.kde.StatusNotifierWatcher")
request("org.kde.StatusNotifierWatcher")
check("registers again when the watcher comes back", spin(lambda: len(registered) > count), True)

# ------------------------------------------------- memory and CPU use
pid = proc.pid
with open(f"/proc/{pid}/status") as f:
    rss = [l.split()[1] for l in f if l.startswith("VmRSS")][0]
with open(f"/proc/{pid}/smaps_rollup") as f:
    roll = {l.split(":")[0]: l.split()[1] for l in f if ":" in l and l.split()[1].isdigit()}
st0 = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
time.sleep(5)
st1 = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
ticks = (int(st1[11]) + int(st1[12])) - (int(st0[11]) + int(st0[12]))
print(f"info memory: VmRSS {rss} kB, Pss {roll.get('Pss')} kB, Private "
      f"{int(roll.get('Private_Clean', 0)) + int(roll.get('Private_Dirty', 0))} kB")
print(f"info CPU ticks used in 5 s idle: {ticks}")
check("RSS below 1 MB", int(rss) < 1024, True)
check("no CPU while idle", ticks, 0)
proc.send_signal(signal.SIGTERM)
proc.wait(5)
err = proc.stderr.read() if proc.stderr else ""
if err.strip():
    print("tray stderr:", err.strip()[-400:])
print("FAILED" if fails else "ALL OK", fails)
sys.exit(1 if fails else 0)
