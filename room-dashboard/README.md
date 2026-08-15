# Room 1 dashboard

Open `index.html` in a browser to use the dashboard. It runs without installing Node.js, npm, or any VS Code extensions.

## ESP32 data format

When the ESP32 is connected to Wi-Fi, make it provide this URL:

`http://ESP32-IP/api/status`

The response must be JSON. Send an event only once when a person completes a direction sequence:

```json
{ "event": "entry", "occupancy": 3 }
```

- `entry` means the object moved **left → right**, so a person entered Room 1.
- `exit` means the object moved **right → left**, so a person exited Room 1.
- `occupancy` is optional. If sent, it overwrites the dashboard’s current count.

Example exit event:

```json
{ "event": "exit", "occupancy": 2 }
```

The ESP32 must send `Access-Control-Allow-Origin: *` with its HTTP response so a browser is allowed to read it.

## Important sensor note

Determining direction needs two sensors placed at different positions along the doorway. The ESP32 decides the direction based on which sensor detects first; the website only displays the `entry` or `exit` event it receives.
