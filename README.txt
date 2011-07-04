JACK-FILE
Jack transport-centric utilities for audio playback

Home page: http://danmbox.github.com/jack-file
Author: Dan A. Muresan (danmbox at gmail dot com)


1. DESCRIPTION:

Jack-File contains two utilities for Jack transport-bound playback:

* file2jack is a gapless audio player that maps files onto the
(optionally periodic) transport timeline. The player achieves
periodicty (looping) without moving the transport: the looped segments
are simply mapped to transport time-points. A picture is worth 10^n
words:

|*****| FILE A | FILE B |******| FILE C | FILE C | FILE C | ...
----------------------------------------------------------------
      |        |        |      |        |        |        |
     10s      20s      30s    40s      50s      60s      70s

file2jack --at 10 -i A.flac --at 20 -i B.flac --at 40 -i C.flac --loop 40

In the example above (which assumes files A, B, C are all 10s long) the
user has requested 10s of silence, then FILE A at 10s, then FILE B at
20s, then after 10 more seconds of silence, FILE C repeated
periodically from 40s to infinity evert 10s.

* jacktransportloop is a simpler way to loop other transport-aware
players: it forces the Jack transport to loop between two time-points.
When the transport reaches the loop endpoint, it is forced back to the
starting point. This will loop any transport-aware player, but
compared to file2jack it will create playback a gap at the loop
endpoint.


2. DEPENDENCIES:

* jack, sndfile; sox (optional -- for formats not supported by
  sndfile, like mp3)

* build dependencies: GNU make, pkg-config, help2man


3. INSTALLING:

# installs in /usr/local:
make install

# installs elsewhere:
make prefix=/opt/jack-file install
PATH="$PATH":/opt/jack-file

# for packagers:
make DESTDIR=build/ prefix=/usr install
cd build; tar cvzf ../jack-file.tar.gz .


4. RUNNING

Call the utilities with --help (or see the manpages) for usage
information.


5. COPYRIGHT:

Copyright 2010-2011 Dan A. Muresan

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
