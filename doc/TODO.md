* Let's work on the UI a bit.
    * Let's give the GUI multiple tabs
      * Devices: shows the basic status of the system, including a single line
        about each enabled Tentacle and each enabled camera -- basically just
        show the device name, the offset, and the time since last seen (we
        should be watching the timecode on the cameras even when we're not
        writing them).  Don't worry about showing the delta between the Mac's
        system time and the device, just show the delta between the device and
        what we're using as the canonical time (whether that's the best device,
        or a mix of multiple good devices, or whatever).
        seeing/syncing.  Only include the enabled devices in this list.
      * Configuration: list all the known devices, with an enable/disable
        checkbox so we can turn them on or off.  List the start/stop/restart
        for the daemon, and the time each daemon has been running, and the
        start at boot.  Add a "pair camera" button, that pops up a UI searching
        for cameras and then lets us trigger a pairing for one.
    * Let's have the "status" CLI look more like the "devices" page.  Add a
      "--verbose" mode to the "status", which shows more information.
      * Be show to show the "last seen" on cameras and timecode boxes in the
        list, that's more important that last synced.
