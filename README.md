<h1 align="center">Asclepio</h1>

<p align="center">
  A self-hosted UPS companion that wakes your homelab servers back up when the power returns.
  <img width="487" height="377" alt="Screenshot 2026-08-31 195438" src="https://github.com/user-attachments/assets/0181607c-94c3-421a-a201-05ddc8fdaf9a" />
</p>

## The problem

Most PCs and servers have a BIOS/UEFI setting called "restore on AC power loss" (or "AC Recovery"), which powers a machine back on automatically once mains electricity returns after an outage. It sounds like the perfect fix — but it doesn't work if your servers are behind a UPS.

Here's why: a UPS like the APC Back-UPS series, when paired with [NUT](https://networkupstools.org/), can trigger a graceful shutdown of your servers as the battery runs low. But at that moment, the UPS itself hasn't fully died — it's still supplying power to its outlets, just running low on charge. So from each server's power supply perspective, AC power never actually went away; the OS just shut down while the outlet stayed live. When mains power comes back and the UPS recharges, there's no AC-loss event to trigger "restore on AC power loss" — the servers simply stay off, because as far as their PSUs are concerned, nothing ever cut off in the first place.

Asclepio solves this without depending on NUT, the UPS, or any server that might itself be down.

## How it works

Asclepio runs on a small microcontroller (originally built for an ESP32-S3) plugged into a **regular wall outlet**, not the UPS. This is the key design decision: unlike the servers behind the UPS, Asclepio genuinely loses power during an outage and reboots when electricity comes back — so its own boot really is a reliable "power is back" signal, unlike the servers' PSUs which never saw a real interruption.

On every check cycle, Asclepio:

1. Pings each configured host on your network
2. If a host doesn't respond, sends it a **Wake-on-LAN** magic packet
3. Logs the result (last 10 checks, in a fixed-size circular buffer — no unbounded memory growth)
4. Serves a small web dashboard showing internet connectivity and recent check history

The first check runs immediately at boot (since boot = power just came back), then repeats on a configurable interval as a safety net.

## Hardware

- An ESP32 board with WiFi (tested on a Freenove ESP32-S3)
- A USB power source connected to a normal wall outlet — **not** the UPS

## Dashboard

Asclepio exposes a lightweight web page on port 80 showing:

- Whether the device currently has internet connectivity
- The last 10 check results, newest first, with server name, time, and outcome (ok / offline / WoL sent)

## Setup

### 1. Install libraries (Arduino IDE Library Manager)

- [ESPping](https://github.com/dvarrel/ESPping) — maintained fork of ESP32Ping
- [WakeOnLan](https://github.com/a7md0/WakeOnLan) by a7md0

`WiFi`, `WiFiUdp`, and `WebServer` ship with the ESP32 core, no extra install needed.

### 2. Configure

Edit the top of `Asclepio.ino`:

```cpp
const char* WIFI_SSID     = "your-ssid";
const char* WIFI_PASSWORD = "your-password";
```

Add your servers (static IP + MAC address of the network interface you want to wake):

```cpp
Homelab servers[] = {
  { "yourHostName",  IPAddress(192, 168, 1, 13), {0x00, 0x00, 0x00, 0x00, 0x00, 0x00} },
  { "yourHostName2", IPAddress(192, 168, 1, 15), {0x00, 0x00, 0x00, 0x00, 0x00, 0x00} },
};
```

Optionally adjust the check interval:

```cpp
const unsigned long CHECK_INTERVAL_MS = 3UL * 60UL * 1000UL; // 3 minutes
```

### 3. Give it a fixed IP

Recommended: set up a DHCP reservation on your router using the MAC address printed to the serial monitor at boot. Alternatively, uncomment the static IP block near the top of the file to set it directly in code.

### 4. Enable Wake-on-LAN on your servers

Getting WoL working reliably usually means checking three layers: BIOS, OS/driver, and — if the interface sits behind a bridge (e.g. Proxmox) — persistence across reboots.

**BIOS/UEFI:**

- Find the power management section and set **Wake on LAN** (sometimes labeled "Wake on LAN/WLAN") to **LAN Only**, not "WLAN Only" — the latter routes wake events through a wireless card if one is present, even unused, and silently breaks wired WoL.
- Disable **Deep Sleep Control** (or "ErP"/"EuP" on some boards). When enabled, it cuts standby power to the network card once the machine is fully off (S5), so it can never receive a magic packet — even with WoL otherwise enabled.
- If the machine has an onboard WiFi/WLAN module you don't use, consider disabling it entirely in BIOS. On some hardware its mere presence — even disconnected — interferes with wired WoL.

**Proxmox (or any Debian-based Linux with a bridged interface):**

Wake-on-LAN must be set on the *physical* NIC, not the bridge (`vmbr0` and similar are virtual and don't support it):

```bash
# find the physical interface enslaved to your bridge
cat /etc/network/interfaces   # look for "bridge-ports <iface>"

sudo apt install ethtool
sudo ethtool <iface> | grep Wake-on   # check current status
sudo ethtool -s <iface> wol g          # enable magic packet wake
```

This setting resets on reboot unless made persistent. Create a systemd service:

```bash
sudo nano /etc/systemd/system/wol.service
```

```ini
[Unit]
Description=Wake-on-LAN for <iface>
Requires=network.target
After=network.target

[Service]
Type=oneshot
ExecStart=/usr/sbin/ethtool -s <iface> wol g

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable wol.service
sudo systemctl start wol.service
```

Reboot and re-check `ethtool <iface> | grep Wake-on` to confirm it survived.

**Plain Ubuntu/Debian with NetworkManager:**

```bash
nmcli connection show                 # find your connection name
sudo nmcli connection modify <name> 802-3-ethernet.wake-on-lan magic
sudo nmcli connection up <name>
```

This is persistent by default under NetworkManager.

### 5. Flash and go

Select **ESP32S3 Dev Module** as the board (or the correct variant for your hardware), upload, and open the serial monitor at 115200 baud to confirm it connects and starts checking.

## Why not just use NUT's own notification hooks?

NUT can notify when power comes back (`ONLINE` event via `upsmon`), and this genuinely works if you run a dedicated NUT master — say, on a Raspberry Pi wired via USB to the UPS — that isn't itself one of the machines the UPS shuts down. That setup can catch the `ONLINE` event and fire off a Wake-on-LAN script just as well.

Asclepio is a simpler alternative to that: no dedicated NUT master, no USB cabling to the UPS, no `upsmon` configuration to get right. It just needs WiFi and a wall outlet. The trade-off is that it doesn't know anything about the UPS's actual state (battery level, load, runtime estimate) — it only knows "did I just reboot, and are the servers responding." For the single purpose of waking servers back up, that's enough; if you want fuller UPS monitoring and alerting, a NUT-based setup still has its place alongside Asclepio, not necessarily instead of it.

## License

MIT
