# Imported patch map

The former ordered patch queue was migrated into one commit per patch, without squashing. This table maps every stable `Switchyard-Patch` ID to its downstream commit. Before publication review, the mechanically imported tree was verified byte-for-byte against the tree produced by applying the former queue to WineHQ revision `0c1585cf5bb9a29a5c480ee04d5529b8fc236044`. The final `0109` commit also includes the compositor correctness and lifecycle corrections documented in its commit message.

| Patch | Commit | Subject |
| --- | --- | --- |
| 0001 | [`2a3c9f842ec4`](https://github.com/jungwuk-ryu/switchyard-wine/commit/2a3c9f842ec4ec5f571199c5cf8c5c3dccd1e46f) | kernelbase: Remember process mitigation policies. |
| 0002 | [`6ff3c344c68c`](https://github.com/jungwuk-ryu/switchyard-wine/commit/6ff3c344c68c7649c155abb52c1a5e7232700284) | conhost: Keep usable font metrics when font selection fails. |
| 0003 | [`8dec1bbd2b2e`](https://github.com/jungwuk-ryu/switchyard-wine/commit/8dec1bbd2b2e6dd0e375da7dc4aa754439d23b2b) | kernelbase: Accept component filter process attributes. |
| 0004 | [`2943d27a1471`](https://github.com/jungwuk-ryu/switchyard-wine/commit/2943d27a147113c77cc4fb8d7505d609f7a3bbf3) | win32u: Enable host Vulkan portability enumeration. |
| 0005 | [`42f6d7c975f9`](https://github.com/jungwuk-ryu/switchyard-wine/commit/42f6d7c975f9d53d2ea82eaf86931524eba99fae) | win32u: Enable host Vulkan portability subset. |
| 0006 | [`6f9210cefabd`](https://github.com/jungwuk-ryu/switchyard-wine/commit/6f9210cefabd13eb74e5dd9c94c7a6a773e61c2e) | ntdll: Export x64 __wine_unix_call thunk. |
| 0007 | [`7087e5c719da`](https://github.com/jungwuk-ryu/switchyard-wine/commit/7087e5c719dacb8fd802438ce7de1e280a52877a) | kernelbase: Bridge native ms_abi thread thunks. |
| 0008 | [`c5658d8b4064`](https://github.com/jungwuk-ryu/switchyard-wine/commit/c5658d8b4064f3e17e2e78610ee200f1c916b932) | ntdll: Register non-native PE code regions. |
| 0009 | [`ea39bcd32ad7`](https://github.com/jungwuk-ryu/switchyard-wine/commit/ea39bcd32ad7dad6207110597e2560087aa402fa) | ntdll: Bridge native Direct3D callback table entries. |
| 0010 | [`67e3ee22f25c`](https://github.com/jungwuk-ryu/switchyard-wine/commit/67e3ee22f25ce70cd6a8a7ed013ac8b29d3d6767) | ntdll: Recover Wine TEB for nested macOS Unix calls. |
| 0011 | [`50714933accf`](https://github.com/jungwuk-ryu/switchyard-wine/commit/50714933accfd8d3b78cf497b64d289bed0a4b5e) | ntdll: Recover Wine TEB for nested macOS syscalls. |
| 0012 | [`575fdf5b2f75`](https://github.com/jungwuk-ryu/switchyard-wine/commit/575fdf5b2f75d8e2b6286dd6610c512f5fb74861) | ntdll: Recover debug TEB for nested macOS callbacks. |
| 0013 | [`8d830f901bdb`](https://github.com/jungwuk-ryu/switchyard-wine/commit/8d830f901bdb13804b3027f7554af00c63928491) | ntdll: Keep PE stack for native callback bridges. |
| 0014 | [`c0c097a8b096`](https://github.com/jungwuk-ryu/switchyard-wine/commit/c0c097a8b096725fe3101e10a6fa5a8db04746f8) | ntdll: Preserve host TSD during native callback nesting |
| 0015 | [`844a5e4bfc94`](https://github.com/jungwuk-ryu/switchyard-wine/commit/844a5e4bfc947c51edb7f05b5af8c9ec69010859) | ntdll: Expand Apple x86_64 native thread stacks |
| 0016 | [`67900fccbccb`](https://github.com/jungwuk-ryu/switchyard-wine/commit/67900fccbccb9599fe3d44d53900f660f42e1916) | ws2_32: Return a basic NLA network lookup result |
| 0017 | [`22433ad1e011`](https://github.com/jungwuk-ryu/switchyard-wine/commit/22433ad1e0112661bd38ae62641c5ae236a5a1f7) | ntdll: Contain host exceptions from native callback bridges. |
| 0018 | [`002788c208cf`](https://github.com/jungwuk-ryu/switchyard-wine/commit/002788c208cf1b16f9fad221e56ca638bcff8f50) | ntdll: Bridge native-to-PE Win32 callback tables. |
| 0019 | [`498a1fc1338a`](https://github.com/jungwuk-ryu/switchyard-wine/commit/498a1fc1338a292720a2e6fc6fff715314fd3cc8) | ntdll: Keep host TSD on nested Unix-call returns |
| 0020 | [`e3a2f6ad4eeb`](https://github.com/jungwuk-ryu/switchyard-wine/commit/e3a2f6ad4eebcd536675d3feb3f5a2a359a20b82) | ntdll: Wrap additional GPTK native callback slots |
| 0021 | [`2a481fceca96`](https://github.com/jungwuk-ryu/switchyard-wine/commit/2a481fceca96888bff0e7dbdd4f3030540706170) | ntdll: Reject null Unix-call handles |
| 0022 | [`464e31a9e39d`](https://github.com/jungwuk-ryu/switchyard-wine/commit/464e31a9e39d6e07a87f179efd4a5abdb48ce49d) | ntdll: Avoid loader lock in Switchyard callback classification. |
| 0023 | [`327d20ec392b`](https://github.com/jungwuk-ryu/switchyard-wine/commit/327d20ec392b60bf1786fe26afced8d187ae1c9b) | ntdll: Read native callback context from the TEB |
| 0024 | [`b477d3dabfcc`](https://github.com/jungwuk-ryu/switchyard-wine/commit/b477d3dabfcc26c9ac4dd8f5dde6db794e3e28b5) | ntdll: Allow GPTK trampoline data sections to execute |
| 0027 | [`0c3e1f8daa2d`](https://github.com/jungwuk-ryu/switchyard-wine/commit/0c3e1f8daa2d93a1ffbb9b0e3cb66d38820a15cb) | ntdll: Route Chromium GPU helpers to Wine graphics modules. |
| 0028 | [`ee94cc326348`](https://github.com/jungwuk-ryu/switchyard-wine/commit/ee94cc3263488ae0ad375c41189c895a3fccbac9) | winemac: Keep offscreen top-level windows visible. |
| 0029 | [`c1a5b99b8987`](https://github.com/jungwuk-ryu/switchyard-wine/commit/c1a5b99b89879953f8c6a90c000513a160ee0ec8) | dwmapi: Report composition disabled to Chromium/CEF. |
| 0030 | [`d17770b26adc`](https://github.com/jungwuk-ryu/switchyard-wine/commit/d17770b26adccb9247c61b56c29214ba94212f5b) | winemac: Redirect Chromium child window surfaces to the root window. |
| 0031 | [`aa53e6347b44`](https://github.com/jungwuk-ryu/switchyard-wine/commit/aa53e6347b443c1d17728a64cc3a5926ce5f0954) | win32u: Rebind Chromium child DCEs to their current surface. |
| 0032 | [`a7888a65e0d1`](https://github.com/jungwuk-ryu/switchyard-wine/commit/a7888a65e0d12d239918f7ebff0b08786bc60764) | win32u: Flush Chromium child window surfaces on unlock. |
| 0033 | [`bd881c7fdcd4`](https://github.com/jungwuk-ryu/switchyard-wine/commit/bd881c7fdcd45be9c0e809a13005b7c60b528c4e) | win32u: Bind Chromium child surfaces before the pixel-format guard. |
| 0034 | [`36b1d6ead1bb`](https://github.com/jungwuk-ryu/switchyard-wine/commit/36b1d6ead1bb4d88126c24a0b1ab7e40b2761033) | winemac/win32u: Recreate Chromium child surfaces for foreign DCEs. |
| 0036 | [`8308ef631c6e`](https://github.com/jungwuk-ryu/switchyard-wine/commit/8308ef631c6ea23a52d243fee3c08dd08f383cf6) | winemac: Create standalone foreign Chromium child surfaces. |
| 0037 | [`907b4f187298`](https://github.com/jungwuk-ryu/switchyard-wine/commit/907b4f18729893d22a27d25bcf541a54d71bda0f) | win32u: Cache foreign Chromium child DCE surfaces. |
| 0038 | [`11d29c74e56d`](https://github.com/jungwuk-ryu/switchyard-wine/commit/11d29c74e56d8ff436975fc245d97117922f9e6e) | winemac: Release foreign Chromium child surface hosts. |
| 0039 | [`b2c204ab203c`](https://github.com/jungwuk-ryu/switchyard-wine/commit/b2c204ab203c2b3c65558050e0eae049468311c0) | win32u/winemac: Preserve constant-alpha layered windows. |
| 0040 | [`cc99f0b2c0bd`](https://github.com/jungwuk-ryu/switchyard-wine/commit/cc99f0b2c0bd29e931d3d69434610174b8ab913d) | winemac: Display the first flushed window surface immediately. |
| 0041 | [`e406fe4ebf65`](https://github.com/jungwuk-ryu/switchyard-wine/commit/e406fe4ebf65fb7714f97dee2728aefc22563bc4) | win32u: Realize selected fonts after late font backend init. |
| 0042 | [`2f1f702b6fff`](https://github.com/jungwuk-ryu/switchyard-wine/commit/2f1f702b6fffd68c064940751f4b261a9b3231b3) | winhttp: Respect missing IE proxy auto-detect settings. |
| 0043 | [`f42f3a3c2f9f`](https://github.com/jungwuk-ryu/switchyard-wine/commit/f42f3a3c2f9fa32b717bdab4428637b4a8519f65) | wined3d: Harden Apple OpenGL capability probing. |
| 0044 | [`7f34a5d97a0e`](https://github.com/jungwuk-ryu/switchyard-wine/commit/7f34a5d97a0efc4761e11fcb68e6bb2ea32efd9b) | wined3d: Skip all Apple RGB16F storage FBO probes. |
| 0045 | [`eecdfd4cee01`](https://github.com/jungwuk-ryu/switchyard-wine/commit/eecdfd4cee01b7b2b5a75df5d4359fa0cd68ef44) | symcrypt: Use scalar SHA-256 on 32-bit x86. |
| 0046 | [`d70054ed35ac`](https://github.com/jungwuk-ryu/switchyard-wine/commit/d70054ed35acc0fa3dbe067ebdc20481353e4058) | ntdll: Guard activation context section output data. |
| 0047 | [`c69585c27f52`](https://github.com/jungwuk-ryu/switchyard-wine/commit/c69585c27f5276bafb6a7d8e9c37aca6dd901e68) | combase: Ignore malformed activation context COM data. |
| 0048 | [`a81e0e71d1cd`](https://github.com/jungwuk-ryu/switchyard-wine/commit/a81e0e71d1cd9f029c012059965f4bc2bb910a39) | user32: Ignore malformed activation context window class data. |
| 0049 | [`b4c6a71ad6b6`](https://github.com/jungwuk-ryu/switchyard-wine/commit/b4c6a71ad6b66e73d9ac5efe7bdb3419d0fd698a) | wined3d: Skip Apple FBO draw probes. |
| 0050 | [`a57d8a9c41fc`](https://github.com/jungwuk-ryu/switchyard-wine/commit/a57d8a9c41fce819b5a8bf35700a0d657d1b40bc) | ntdll: Use the WOW64 host stack from R14 for macOS exceptions. |
| 0051 | [`67179bdccb25`](https://github.com/jungwuk-ryu/switchyard-wine/commit/67179bdccb25da3dcecc1a40a5eab18ee0766350) | wine: Add Chromium/GPTK launcher login graphics compatibility. |
| 0052 | [`d196d1458c64`](https://github.com/jungwuk-ryu/switchyard-wine/commit/d196d1458c6434282008265bcb34979428618d9b) | ntdll: Recover TEB for critical-section owner checks |
| 0053 | [`82ec96dc37d7`](https://github.com/jungwuk-ryu/switchyard-wine/commit/82ec96dc37d7a0106205f31d788583dbd14a69a7) | winemac: Front foreign Chromium child host surfaces. |
| 0054 | [`5da18ba3643d`](https://github.com/jungwuk-ryu/switchyard-wine/commit/5da18ba3643d79f7d6952c4391eb8d2a90c2c0e1) | winemac: Let foreign Chromium overlays pass mouse input through. |
| 0055 | [`d17765270bae`](https://github.com/jungwuk-ryu/switchyard-wine/commit/d17765270bae666c108186c39ccaa5d8e6ab8bdd) | winemac: Re-front foreign Chromium overlays on flush. |
| 0056 | [`86303459b505`](https://github.com/jungwuk-ryu/switchyard-wine/commit/86303459b5052915a4111afcf2c9fab06e667ba4) | winemac: Fade blank Chromium owner hosts behind overlays. |
| 0057 | [`d755b3f5183a`](https://github.com/jungwuk-ryu/switchyard-wine/commit/d755b3f5183a1af92273379e0d6b71f798f087fe) | winemac: Host foreign Chromium child surfaces as remote layers. |
| 0058 | [`be9bdd0c4f15`](https://github.com/jungwuk-ryu/switchyard-wine/commit/be9bdd0c4f1592945d1299c9ed04869f920a99d5) | winemac: Merge foreign Chromium child layers into owner hosts |
| 0059 | [`5bb1b359fe58`](https://github.com/jungwuk-ryu/switchyard-wine/commit/5bb1b359fe58bd6a4910cbc17eb4cc974f8c6d79) | winemac: Query remote layer hosts through owner windows |
| 0060 | [`21942211c875`](https://github.com/jungwuk-ryu/switchyard-wine/commit/21942211c8750aa9c4c3046bd4765970fe2494c8) | winemac: Clear blank owner backings behind remote layers |
| 0061 | [`5fabc6555ef0`](https://github.com/jungwuk-ryu/switchyard-wine/commit/5fabc6555ef06d89f88b9b3ffb304a7912b60a62) | winemac: Keep hosted CEF owners opaque between layer cycles |
| 0062 | [`ae3e313c8561`](https://github.com/jungwuk-ryu/switchyard-wine/commit/ae3e313c85610ef1f8da5fad8c263c876392cc08) | winemac: Remember CEF remote hosts across surface recreation |
| 0063 | [`0892c9d15bbc`](https://github.com/jungwuk-ryu/switchyard-wine/commit/0892c9d15bbca1a092e1c1dc503eedff201559bd) | winemac: Disable implicit animations for CEF layer hosts |
| 0064 | [`41934cf381c7`](https://github.com/jungwuk-ryu/switchyard-wine/commit/41934cf381c70446bd95a4c5415d41edf1f70ec0) | winemac: Retire CEF remote layer hosts after replacements |
| 0065 | [`66d1462c0bbb`](https://github.com/jungwuk-ryu/switchyard-wine/commit/66d1462c0bbbcda613bdffee539ec5073fc4d915) | winemac: Clear blank CEF owners instead of fading windows |
| 0066 | [`186acc3d53bc`](https://github.com/jungwuk-ryu/switchyard-wine/commit/186acc3d53bc416bfa92992f2ad2bd9f0c8ade6a) | winemac: Disable implicit animations for remote image layers |
| 0067 | [`c5794d38bd29`](https://github.com/jungwuk-ryu/switchyard-wine/commit/c5794d38bd29bb0a0e03bdfd45bb216af5742dc9) | winemac: Stabilize Chromium child layer composition |
| 0068 | [`24198bd95507`](https://github.com/jungwuk-ryu/switchyard-wine/commit/24198bd95507f65828fb0e172cfe8aeb54ff00b9) | combase: Pass AppID ServiceParameters to local services. |
| 0069 | [`dc121b9a7f56`](https://github.com/jungwuk-ryu/switchyard-wine/commit/dc121b9a7f563a8829e16b594ea2fcddc0bc7f54) | server: Allow registry DACL updates by token access. |
| 0070 | [`6864ace3b543`](https://github.com/jungwuk-ryu/switchyard-wine/commit/6864ace3b543123a05e38a4f1f37e3b783490b6d) | rpcrt4: Treat IUnknown-only typelib parents as IUnknown. |
| 0071 | [`c6187814f32b`](https://github.com/jungwuk-ryu/switchyard-wine/commit/c6187814f32b8e0a7d96e727978c43d9ea8241d9) | rpcrt4: Support typelib LPSTR and LPWSTR parameters. |
| 0072 | [`e064d009f713`](https://github.com/jungwuk-ryu/switchyard-wine/commit/e064d009f713c58857c27f5b952858ce8d5cba35) | winhttp: Drain gzip parent streams before reuse. |
| 0073 | [`a6f9d10659a2`](https://github.com/jungwuk-ryu/switchyard-wine/commit/a6f9d10659a25f62718f134e87f19c329da0133a) | ntdll: Resolve app-local assembly DLLs from manifest directory. |
| 0074 | [`222b6565091c`](https://github.com/jungwuk-ryu/switchyard-wine/commit/222b6565091c9c524ac69501dd3c54694475050e) | ntdll: Activate executable manifests while resolving imports. |
| 0075 | [`8858e54514f6`](https://github.com/jungwuk-ryu/switchyard-wine/commit/8858e54514f6349d05512da3c4f4c61691a0f058) | winemac: Gate full-root Chromium layer suppression on hosted viewports. |
| 0076 | [`a4288a93850a`](https://github.com/jungwuk-ryu/switchyard-wine/commit/a4288a93850af15fde0058d248a59e4eb5914671) | dcomp/dxgi: Support Chromium DComp texture presents. |
| 0077 | [`1b2f0f482681`](https://github.com/jungwuk-ryu/switchyard-wine/commit/1b2f0f4826810ea2ff5ef74bee15b39e853eed68) | winemac: Preserve solid Chromium DComp content layers. |
| 0078 | [`18a3b06b080f`](https://github.com/jungwuk-ryu/switchyard-wine/commit/18a3b06b080f9d1ee9f55dd108a1f040bad5ad84) | winemac: Keep Chrome owner backing before hosted viewports. |
| 0079 | [`d06601d60139`](https://github.com/jungwuk-ryu/switchyard-wine/commit/d06601d601395cbbc8c61fb5978b936fa7d46905) | dcomp: Flush RGBA conversion before compositor readback. |
| 0080 | [`39a0c797e14b`](https://github.com/jungwuk-ryu/switchyard-wine/commit/39a0c797e14b596197e5cc6dde35996e0bfc83cd) | d3d11: Preserve GDI-compatible access on wrapped textures. |
| 0081 | [`c386fb0abf7e`](https://github.com/jungwuk-ryu/switchyard-wine/commit/c386fb0abf7e632d895399e561b6ab249ef262c6) | wined3d: Synchronize Vulkan GetDC readback buffers. |
| 0082 | [`7b8b6660d71b`](https://github.com/jungwuk-ryu/switchyard-wine/commit/7b8b6660d71b92cb334adb51a648cd37ddeeed53) | dcomp: Respect dynamic texture alpha during composition. |
| 0083 | [`bcc65e4b3854`](https://github.com/jungwuk-ryu/switchyard-wine/commit/bcc65e4b38541a31293edc542b86061772f817aa) | dxgi: Use presented flip buffers for DComp readback. |
| 0084 | [`0d7e2ed89d45`](https://github.com/jungwuk-ryu/switchyard-wine/commit/0d7e2ed89d450b6e048a990022356775c13569e2) | winemac: Keep Chromium root backing until hosted viewport. |
| 0085 | [`c393382d3625`](https://github.com/jungwuk-ryu/switchyard-wine/commit/c393382d36256689105508521e095db005d95c25) | dcomp: Correct dynamic texture vtable order |
| 0086 | [`316f0f822524`](https://github.com/jungwuk-ryu/switchyard-wine/commit/316f0f822524adab73c930e148ef9f534803b73d) | dcomp: Compose target DC before flushing clear frames. |
| 0087 | [`ad0e596f6023`](https://github.com/jungwuk-ryu/switchyard-wine/commit/ad0e596f60239c235bab0f32f43fb732a2e12669) | winemac: Align hosted Chromium layers to root client frame |
| 0088 | [`7b3578db7152`](https://github.com/jungwuk-ryu/switchyard-wine/commit/7b3578db71527ae8881246d317be25357dbe3171) | winemac: Prefer DComp root composition for Chromium |
| 0089 | [`353bbfe4beb2`](https://github.com/jungwuk-ryu/switchyard-wine/commit/353bbfe4beb211ec4b23cbe3e6361dc616ae1cbd) | dcomp: Compose targets through a memory DC |
| 0090 | [`c6dba3cc9b8e`](https://github.com/jungwuk-ryu/switchyard-wine/commit/c6dba3cc9b8ee548b1b8aefbb2a00faf42fec28c) | winemac: Compose Chromium child surfaces into the DComp root |
| 0091 | [`a8c08c092dc8`](https://github.com/jungwuk-ryu/switchyard-wine/commit/a8c08c092dc86b10e9a65c87e5d9324a2d05819a) | winemac: Relay Chromium root surfaces through the DComp owner |
| 0092 | [`49a7577a8cdf`](https://github.com/jungwuk-ryu/switchyard-wine/commit/49a7577a8cdf8665ae67d56bd0c48438ad8637b2) | dcomp/winemac: Composite owned Chromium popups through the root |
| 0093 | [`94b9a3812536`](https://github.com/jungwuk-ryu/switchyard-wine/commit/94b9a381253631f26b7ab5aa7f92abfce2f5859a) | winemac: Preserve masked Dock icon transparency |
| 0094 | [`323345774bc9`](https://github.com/jungwuk-ryu/switchyard-wine/commit/323345774bc947f2b489a89b92df9083558c1243) | wined3d: Stop forcing Vulkan for Chromium helpers |
| 0095 | [`ef9882a625b0`](https://github.com/jungwuk-ryu/switchyard-wine/commit/ef9882a625b0144a8ea74d4e3457ff573197c2f9) | winemac: Preserve non-DComp CEF root layers |
| 0096 | [`9bec32797633`](https://github.com/jungwuk-ryu/switchyard-wine/commit/9bec3279763331310bfe40cd757aec1dbab722b4) | dcomp: Preserve alpha in dynamic texture caches. |
| 0097 | [`3fbd2cc2e001`](https://github.com/jungwuk-ryu/switchyard-wine/commit/3fbd2cc2e0019457af9b5d1e073eb643738a31dd) | winemac: Keep full-root CEF hosts below viewports |
| 0098 | [`93633f743747`](https://github.com/jungwuk-ryu/switchyard-wine/commit/93633f743747ae9ed04a773f525d5fe10ae2f553) | dcomp/winemac: Retire owner-composed Chromium popup windows. |
| 0099 | [`59c4d329010a`](https://github.com/jungwuk-ryu/switchyard-wine/commit/59c4d329010a68f7d5ed6d555ce9fffe540c6109) | winemac: Root-compose Chrome render host trees without DComp props. |
| 0100 | [`8f7ad740a2ab`](https://github.com/jungwuk-ryu/switchyard-wine/commit/8f7ad740a2ab54b07103558a692c40855addef13) | winemac: Use borderless Chrome custom frames |
| 0101 | [`b32a1210b7be`](https://github.com/jungwuk-ryu/switchyard-wine/commit/b32a1210b7be48e4fd3891218e1b7233d208258e) | cryptowinrt: stub AppCapabilityAccess |
| 0102 | [`d4abebb04b03`](https://github.com/jungwuk-ryu/switchyard-wine/commit/d4abebb04b03568cd70c0ca9dd7e1fe2988b77f3) | winemac: preserve Chrome content during window moves |
| 0103 | [`20e671ad68c9`](https://github.com/jungwuk-ryu/switchyard-wine/commit/20e671ad68c98fbfdb41aea24661bb9e23e1e0a3) | appcontainer: support Chromium sandbox profiles |
| 0104 | [`88908ef7bccb`](https://github.com/jungwuk-ryu/switchyard-wine/commit/88908ef7bccbe79b6d02b0b25ff0540425eefde6) | mediacontrol: require WinRT stream dependencies |
| 0105 | [`cbce387b7d29`](https://github.com/jungwuk-ryu/switchyard-wine/commit/cbce387b7d2988b5f07a6f13e0db624c0ebca6fd) | winemac: Host cross-process Chromium client surfaces |
| 0106 | [`a39a7400df46`](https://github.com/jungwuk-ryu/switchyard-wine/commit/a39a7400df4636afd9ac2c5d81f1b38be8dbf834) | dcomp,winemac: stabilize Chrome custom-frame composition |
| 0107 | [`dbb0a3fca83c`](https://github.com/jungwuk-ryu/switchyard-wine/commit/dbb0a3fca83c1868f9ef92432b5aa9c45d22738b) | winemac: route input through foreign Chromium hosts |
| 0108 | [`1d21d9c0693a`](https://github.com/jungwuk-ryu/switchyard-wine/commit/1d21d9c0693a35edc0a1efb90b9debe29cd92634) | winemac: keep foreign Chromium hosts click-through |
| 0109 | [`b9daccff113c`](https://github.com/jungwuk-ryu/switchyard-wine/commit/b9daccff113c5b3dfbb8f5aacdb5b10e2af7c719) | fix: unify Win32 GPU surfaces under native roots |
