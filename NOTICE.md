# Notices

## This program

M-Strata (Multiengine Strata, formerly Secure Strata)
Copyright (C) 2026 shift-f10 and the Secure Strata contributors

This program is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General
Public License as published by the Free Software Foundation, version 3 of the License (AGPL-3.0-only).

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
details. You should have received a copy of it along with this program, in [`LICENSE`](LICENSE); if not, see
<https://www.gnu.org/licenses/>.

## Additional permission under GNU AGPL version 3 section 7: MiniMax-M3.1-Flash

If you modify this program, or any covered work, by combining it with the MiniMax-M3.1-Flash chat template text, or
with the MiniMax-M3.1-Flash model weights, which are distributed under the MiniMax Community License (or a modified
version of that template text or those weights), the licensors of this program grant you additional permission to
convey the resulting work. The MiniMax material keeps its own license, whose terms apply to it (and not this
program's): non-commercial use and redistribution with its notice; commercial use requires "Built with MiniMax
M3.1-Flash" to be shown and a notice to, or written authorization from, MiniMax.

These files hold MiniMax's template text, in whole or in part, and are under the MiniMax Community License
([`engine/third_party/maya/LICENSE.minimax-m31`](engine/third_party/maya/LICENSE.minimax-m31), Copyright (c) 2026
MiniMax) for that text:

- `engine/third_party/maya/tools/mm3/chat_template.jinja` (Project Maya's copy, MIT for Maya's own addition)
- `secure_frontend/src/minimax_prompt_renderer.cpp` (the template's text constants)
- `secure_frontend/tests/fixtures/minimax_prompts.json` (prompts rendered from the template)

An out folder with the Maya families carries the template and its license in `engine/templates/minimax-m3.1/`.

## Code this program builds on (MIT)

Much of this program derives from Strata. Its license notice, kept as its terms require:

```
MIT License

Copyright (c) 2026 Niko1221 and the Strata contributors

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

## The WebView2 SDK (strata-window on Windows)

A Windows build with the window (`make.py --with window`) links Microsoft's WebView2 loader
(`WebView2LoaderStatic.lib`, from the Microsoft.Web.WebView2 NuGet package, version 1.0.4314.35) into
`strata-window.exe`. Its license notice, kept as its terms require:

```
Copyright (C) Microsoft Corporation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

   * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.
   * The name of Microsoft Corporation, or the names of its contributors 
may not be used to endorse or promote products derived from this
software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

Third-party code keeps its own license beside it: `upstream/LICENSE` (Strata, the unmodified submodule),
`engine/third_party/*/LICENSE` (Project Maya, ggml, exllamav3, beellama.cpp), `engines/maya-ref/LICENSE`,
`secure_frontend/web/LICENSE.strata-ui.txt`. [`docs/PROVENANCE.md`](docs/PROVENANCE.md) is the full record.
