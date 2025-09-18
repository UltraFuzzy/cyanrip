[cyanrip](#cyanrip) patches
===========================
- [x] Prevent constant spurious disc eject detection on macOS.
- [x] Report peaks in terms of sample peak relative amplitude for comparison EAC and XLD and identification of distinct masterings.
- [x] Detect pregaps on physical CDs. (libcdio's cdio_get_track_pregap_\[lba|lsn\]() only works for CD image files.)
- [ ] Verify subchannel Q reads with their CRC16s.
