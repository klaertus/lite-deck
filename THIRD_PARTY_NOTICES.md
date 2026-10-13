# Third-party notices

lite-deck is licensed under the GNU General Public License, version 3 (see [LICENSE](LICENSE)). It
includes or uses the third-party components below, each under its own licence.

## Compiled into lite-deck

### nlohmann/json 3.11.3

- Source: <https://github.com/nlohmann/json>
- Vendored file: `third_party/nlohmann/json.hpp`
- Licence: MIT

### cpp-httplib 0.54.1

- Source: <https://github.com/yhirose/cpp-httplib>
- Vendored file: `third_party/httplib/httplib.h`
- Licence: MIT

## Used by the tests only (not distributed)

### doctest 2.4.11

- Source: <https://github.com/doctest/doctest>
- Vendored file: `third_party/doctest/doctest.h`
- Licence: MIT

## Distributed with lite-deck (unmodified)

### PawnIO module `AMDFamily17.bin` (PawnIO.Modules 0.2.11)

- Used by `lite-deck-sensors.exe` to read the CPU temperature of AMD family 17h-1Ah processors.
- Source: <https://github.com/namazso/PawnIO.Modules> (module source: `AMDFamily17.p`)
- Binary: the signed module from release 0.2.11,
  <https://github.com/namazso/PawnIO.Modules/releases/download/0.2.11/release_0_2_11.zip>,
  SHA-256 `DAE74615761B78BDF064DFB3E136252DDCC6FC727D88F14738D0E5800D427A91`
- Vendored files: `third_party/pawnio-modules/AMDFamily17.bin` and `third_party/pawnio-modules/SOURCE.txt`
- Licence: GNU Lesser General Public License, version 2.1. The full text is in
  `third_party/pawnio-modules/COPYING`, and it ships next to the module as `AMDFamily17.COPYING.txt`.

### PawnIO driver installer `PawnIO_setup.exe`

- Copyright (C) namazso.
- Not stored in this repository. `scripts/package.ps1` downloads the latest official release from
  <https://github.com/namazso/PawnIO.Setup/releases> and checks its Authenticode signature
  (`CN=namazso.eu`). The installer checks the signature again before it runs the setup, and runs it
  only with the user's consent.
- PawnIO driver source: <https://github.com/namazso/PawnIO>. Installer source:
  <https://github.com/namazso/PawnIO.Setup>. Project site: <https://pawnio.eu/>.
- Licence: the PawnIO driver is licensed under the GNU General Public License, version 2 or (at your
  option) any later version. See the upstream repositories for the licence terms of each component.
- At run time, `lite-deck-sensors.exe` loads `PawnIOLib.dll` from the PawnIO installation folder.
  lite-deck does not distribute that DLL.

## Online services

The `weather`, `sun` and `crypto_gold` modules use public web APIs: Open-Meteo
(<https://open-meteo.com/>, weather data under CC BY 4.0) and CoinGecko (<https://www.coingecko.com/>).
They are services, not components distributed with lite-deck. Their terms apply to their use. See
the privacy section of the [README](README.md#privacy-and-network).

## MIT licence texts

### nlohmann/json

```
MIT License

Copyright (c) 2013-2023 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### cpp-httplib

```
The MIT License (MIT)

Copyright (c) 2026 Yuji Hirose

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### doctest

```
The MIT License (MIT)

Copyright (c) 2016-2023 Viktor Kirilov

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
