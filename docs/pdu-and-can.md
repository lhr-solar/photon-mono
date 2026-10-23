---
title: PDU and CAN
---

# PDU and CAN

These two tabs are how you check a single device when a Mechanical chart is empty.

## PDU

PDU lists high-side channels 0 through 16 from `PDU_Status_Arr`. Each channel can show voltage, current, whether it is enabled, and whether it is in fault.

If the page says the catalog does not have `PDU_Status_Arr` yet, this run's DBC decode does not include that message. The channels are not hidden. They are absent. Ask whoever owns the log which DBC was applied before you treat a blank PDU page as a dead box.

Mechanical uses this tab when a sensor that is powered from the PDU is flat on every chart and the rest of the car is live. Find the channel, and read enabled and fault next to the voltage.

The chart layout buttons on this page are the same **Unlock layout**, **Undo**, and **Reset layout** as Mechanical.

## CAN

CAN is the message list from the DBC, plus whatever is on the bus.

The header counts messages, signals, and how many ids are on the bus. The search box takes a name, a signal, or an id. Open a message to see its signals and the latest value.

Use it when a Plots search finds nothing:

- The message is missing from the list. It is not in this DBC.
- The message is in the list and the rate is 0 during a moving part of the run. It was not on the bus then.
- The message is in the list, the rate is healthy, and the chart is still empty. The plot is looking at the wrong 15 seconds, or the signal name does not match. Check [Signal names](/signals.html) and the playhead.

Raw rows under the decoded list are ids the catalog does not name. A sensor that only shows up there is not decoded, so Mechanical cannot plot it until the DBC has that message.

Replay still works on this tab. The rate on a pinned run is the history rate, not a live blink.
