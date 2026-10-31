# Commit IDs before and after the 2026-10-31 rewrite

On 2026-10-31 the owner had `main` rewritten so that no commit credits an agent ("Rewrite m-strata"): every commit
lost its `Co-Authored-By:` trailer and kept its tree, author, committer and dates. Every commit ID changed. Commit
IDs cited in `docs/AGENT_HANDOFF.md` and elsewhere before its 2026-10-31 z87 entry name the old commits; this table
gives the new one for each, oldest first. (IDs cited before 2026-10-27 name commits of the older local branch
`history-before-2026-10-27`, as that entry says.)

| Old | New | Subject |
| --- | --- | --- |
| `8e9cef9533` | `26f1f45726` | M-Strata (Multiengine Strata): the code as of 2026-10-28 |
| `6f28cbbc00` | `9d253fd2a0` | Gemma 4 web search, MTP on Qwen3.8 cards, downloads that keep their subfolder, options on the data folder |
| `cb6411ff1b` | `4809dcb0ae` | Pictures on Qwen cards: the vision files download, upstream's strata-vision beside the engine, picture attachments |
| `4538a356d6` | `f61c80ea7a` | Several GPUs as upstream runs them: a GPUs choice on every card, GLM's split, DeepSeek's tensor-parallel pairs |
| `297be6f553` | `0166aad492` | The page on a phone: folder paths on their own line, the About's engines as labelled blocks, toasts clear of the edges |
| `4072a7a777` | `2c293c4747` | Read upstream's picture encoder answer: the token count is the first field after OK |
| `39141d3a18` | `7295323b1f` | Write a picture's generate line in upstream's order: the embeddings file, then the ids |
| `ca9d7f7ba9` | `29f2d7c727` | Pictures checked on the GPU: Qwen3.8 IQ3_S read a test picture correctly in about 4 s |
| `298349a35b` | `6a861beb8f` | A Settings tab: the API key, the server's own settings, and the settings that were under Models |
| `336e179404` | `2c0778f64c` | strata-window: the server in a window with a tray icon |
| `ccf4f0513e` | `4370103e7d` | make --with window builds strata-window, not only installs it |
| `6803934be0` | `87b4fb62d1` | strata-window opens the page on the Monitor |
| `0299fead7c` | `d1b853741b` | One-step install (install.bat, install.sh) and a step-by-step install guide |
| `04bde477ec` | `f214ddd313` | Web search for DeepSeek-V4.1: its DSML tool form, as Maya renders and reads it |
| `d21e69ba29` | `365ef73bf2` | Web search for MiniMax-M3.1: its namespaced XML tool form, as Maya renders and reads it |
| `453339aff5` | `45080f5e78` | MiniMax search on the real pack: the call tags are special tokens too |
| `9e50b8adb7` | `7c8bc592d0` | Calibration for every model, each with its reference tool's steps |
| `378b540b6c` | `6d9393a286` | The server is strata-unified-server |
| `c9cfdc1d0a` | `855b16313c` | Gemma 4 drafts with its MTP model: same output, about 1.2x faster |
| `ab29ee2aaf` | `22833e0042` | strata-tui, Linux builds again, new download cards, an upstream Monitor |
| `009b831d9a` | `31a60747ed` | Model stops wait for a big engine, Remove card, other versions, download reasons |
| `a1aad9ddc6` | `678d08b6ca` | An installed download without its card offers Set up |
| `356c81922a` | `7c72bbae42` | An installed download a folder card runs reads Downloaded again |
| `b40f4e1695` | `997214cf70` | Gemma 4: a NORMAL <eos> is still the marker |
| `686011381b` | `c5eb17ea67` | Gemma 4 prompts follow the newer, official chat template |
| `60ea86a570` | `4dd2fbc7e3` | Search chats know the date and keep earlier searches |
| `4aa763e2fc` | `c993910317` | Cards carry their model's sampling; the chat follows the card; point a card at its MTP draft layer |
| `680baad375` | `08a938012b` | Qwen3.8's MTP draft layer, built natively and fetched with each Qwen3.8 download |
| `fa153ed832` | `d1ce11e009` | GLM cards offer the RAM their engine leaves free |
| `b16ba68263` | `b1c1f318f5` | Handoff: the RAM headroom option measured live |
| `3bda0003bf` | `bd659cbb82` | install.ps1 parses again; GLM-5.3 cards calibrate |
| `f18c1d2649` | `66925d4d4d` | make finds cl when run from PowerShell |
| `baa91f2a81` | `5f5d6ba795` | make builds for this PC's supported card types; RTX 50's FP4 units build |
| `0f1869f70f` | `24f3f059f9` | An MTP draft layer folder can be made private |
| `c8a92f12a4` | `a0e7dc75fa` | strata-window shows the server's log |
| `5ea3cab8db` | `0d52431579` | The engine's own output can go to the log; answers may take 30 minutes |
| `400776a45e` | `f930b06f93` | The Monitor reads a GPU whose nvidia-smi answers a placeholder |
| `3239b91018` | `055563995b` | A debugging build asks before it starts the server |
| `ac20d7b7cb` | `8276d7ec07` | The Settings tab scrolls |
| `c9e06d393d` | `ede3ef21b2` | Starting an uncalibrated model asks to calibrate first |
| `1b8516ee45` | `e537668da7` | The Monitor names a calibration while it runs |
| `44bec0b2f2` | `241396db39` | Model folders may be on network shares (experimental); a Qwen3.6 card says where its draft layer is |
| `d0092272af` | `be890b33d1` | Troubleshooting only in a debugging build; the installers ask what to build |
| `3cb32db727` | `e34a043254` | The tray lists cards that have decimal sampling options |
| `9dc20e5ea1` | `150f0168f9` | The README shows the window app, its tray menu and the TUI |
| `1e2c8a2955` | `8cf8b46808` | The README has a compatibility table |
| `2a658cb72c` | `2bd4d35ecd` | A calibration with an error writes a report for a bug report |
| `85f808bac3` | `de6ec7c25c` | The compatibility table has the RTX 5090 PC's two Qwen rows |
| `528ee322d9` | `60de4e5226` | Models has a Compatibility tab to copy this PC's results |
| `1d85465c65` | `1340f3855b` | Standing rule: workers are checked |
| `f5f7829785` | `a13cafd11d` | install.sh finds GCC 13 before it installs anything; Ubuntu 22.04 works |
| `4489d36749` | `20b0cef806` | The engine and the frontend build on Linux with GCC 13 |
| `417dcd9821` | `db9b9ef1c3` | The launcher builds on GCC 13 (main's own Linux install) |
| `72211ad192` | `82930cafec` | The compatibility table has the Azure Standard_NC4as_T4_v3 |
| `629c45ab54` | `7f3c0830c1` | strata-tui shows a message whole, before the keys |
| `4a5b4db795` | `3199587711` | A server bound to every address takes its own names |
| `cab1b71fdf` | `ad82d6f291` | The download children create owner-only files |
| `ba57904d08` | `02802eff0e` | A Qwen pack card starts in upstream's low-RAM mode when RAM is short |
| `d0ec8ed54c` | `6ee2f90307` | A chat request may ask for a JSON object |
| `28dcf4f0e6` | `c485016780` | A card can be pinned to start with the server |
| `018ba9ce85` | `71e26523dd` | Every card offers Keep loaded |
| `b9ba7f6f9a` | `800929e506` | A card kept loaded is warmed |
| `e23c4ab4a8` | `c75f6b57e0` | A card kept loaded is started again |
