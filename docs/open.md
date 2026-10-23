---
title: Open the page
---

# Open the page

Open the team Photon page and sign in with the login your lead gave you. A login prompt means the site is up.

On the car computer, the same page is [http://127.0.0.1:8080/photon.html](http://127.0.0.1:8080/photon.html).

## If the browser complains about the certificate

Firefox `SSL_ERROR_RX_RECORD_TOO_LONG`, or Chrome marking the site Not Secure while you are on the UT VPN, means campus filtering is intercepting the connection. Leave the VPN or switch to a hotspot, then reload.

A server that is actually down still shows a normal lock icon, then an HTTP 502. Reload once. If it stays on 502, the page is not being served and someone who runs the car computer has to look at it. Do not keep refreshing a login prompt. That prompt is the page working.

## What you should see

The top bar is Pit, Dashboard, Plots, Mechanical, PDU, Live View, and CAN. The left edge has a vertical **LOGS** tab. Open that before you trust an empty chart. Next is [Logs](/logs.html).
