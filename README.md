# JavaServerUWP

runs a headless minecraft java server inside a uwp app on an xbox series s in dev
mode. the jvm is embedded in-process (same trick as my bandit launcher, but
server side) and the console renders to the tv with direct2d. paper is the main
target, fabric and vanilla also work.

## status

paper 1.21.11 boots and runs on a series s: binds 25565, generates worlds,
players join over lan, console commands work. the fabric path still builds too.

## build

needs vs build tools (msvc + windows sdk) and amazon corretto jdk 21.

    .\build.ps1 -Paper -SkipDownload

- `-Paper` builds the paper server, drop it for the fabric build.
- run once without `-SkipDownload` to pull paper.jar, the jna native and the
  libraries. after that `-SkipDownload` skips the network.
- output is `out\MC.Server.appx`, signed with a dev cert, deploy to the xbox.

## the uwp gotchas

most of the work is making a normal jvm survive the dev mode container. the
non-obvious bits, all explained at the call sites in `MC.Server\App.cpp`:

- stdout/stdin only reach the jvm if you SetStdHandle after the dup2. the jvm
  reads the os handles at vm init, not the crt fds.
- dlls only load from the read-only install dir, not a writable one, so
  jnidispatch and the vc runtime are hand-loaded from the package in order.
- toRealPath is denied in the sandbox, so jdk.zipfs and java.base are patched
  via --patch-module to fall back to toAbsolutePath.
- tls needs keystore.type=pkcs12 set explicitly because the security override
  drops the defaults.
- oshi can't read hardware (wmi/com is blocked), so that crash-report noise is
  silenced in the log config.

## layout

- `MC.Server\App.cpp` - the whole host in one file: jvm boot, options, console,
  lifecycle.
- `build.ps1` + `scripts\` - build pipeline.
- `patch\` - the jdk.zipfs / java.base toRealPath patches and the fabric
  loaderutil patch, compiled at build time.
- `log_configs\server-uwp.xml` - log4j config the server runs with.
- `compat_mod\` - fabric mixin mod for the fabric path.
- `xbox_security.properties` - java.security override shipped into the jre.
