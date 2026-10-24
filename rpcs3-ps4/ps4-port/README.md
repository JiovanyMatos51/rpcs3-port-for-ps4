# RPCS3 PORT FOR PS4 - by Roberix Studio & Mifi Studios

Port do [RPCS3](https://rpcs3.net), o emulador de PlayStation 3, para consoles
PS4 desbloqueados com GoldHEN. O núcleo de emulação é o código original do
RPCS3, compilado para o PS4 com o toolchain OpenOrbis; o port acrescenta o
frontend do console, os adaptadores nativos (memória, vídeo, áudio, controles,
rede, USB) e um launcher no estilo XMB.

Versão atual: **0.40** (TITLE_ID `RPCS30010`).

Projeto não oficial, sem vínculo com a Sony nem com a equipe do RPCS3.
O pacote não inclui jogos, firmware, chaves ou licenças: use apenas cópias
dos seus próprios jogos.

## Recursos

- **Recompiladores LLVM de PPU e SPU** do RPCS3 rodando no próprio PS4
  (LLVM fixado pelo submódulo do RPCS3), além do SPU ASMJIT e dos
  interpretadores.
- **Vulkan** pelo driver Mesa RADV (renderizador `VKGSRender` original).
- **OpenGL** pelo Mesa EGL + Zink sobre o RADV (renderizador `GLGSRender`
  original).
- **Configuração automática**: ao abrir um jogo é aplicado o perfil indicado
  para o PS4 e as correções conhecidas de cada título. Se um jogo falhar, a
  próxima inicialização se ajusta sozinha (menos threads de compilação, menos
  VRAM). Configurações próprias por jogo continuam possíveis.
- **Até 4 jogadores**, um controle DualShock 4 por usuário logado no PS4, com
  a cor da barra de luz de cada jogador.
- **Online pelo RPCN** (servidor do próprio RPCS3): importação do `rpcn.yml`
  do RPCS3 de PC, login e criação de conta pelo teclado da tela, UPnP
  opcional.
- **Launcher XMB**: lista de jogos (pastas, ISO, PKG), instalação de PKG de
  PS3 (jogos, updates, DLC), importação de licenças `.rap`, configurações
  globais e por jogo, informações do sistema. Usa as fontes e os ícones do
  firmware do PS3 copiado para o console, quando disponíveis.
- **Disco de PS3 no leitor do PS4**: o disco é lido pelo servidor FTP do
  próprio GoldHEN (só leitura) e instalado uma vez em `/data/rpcs3/games`;
  a chave do disco é buscada sozinha (IRD, como o PS3 Disc Dumper do PC) e
  a cópia já sai decriptada. Círculo cancela e a instalação continua depois
  de onde parou.
- Retorno do jogo para a XMB sem fechar o aplicativo (L1 + R1 + Triângulo por
  1 segundo, ou L1 + R1 + Options por 2 segundos).
- **Patches de jogos** como no PC: importação (FTP ou pendrive), download de
  rpcs3.net por HTTPS verificado e lista por jogo (R1).
- **Cheats** (códigos Artemis/NetCheat e o `cheats.yml` do RPCS3 de PC): R2
  sobre um jogo na XMB, ou o menu do botão PS dentro do jogo.
- **Idiomas**: XMB, janelas, avisos e configurações em Português (Brasil),
  English, Español e العربية (árabe, da direita para a esquerda). Segue o
  idioma do PS4, ou escolha em Configurações > "Idioma · Language · اللغة".
- **Configurações do RPCS3 completas**: as abas do RPCS3 de PC (CPU, GPU,
  Áudio, E/S, Sistema, Rede, Avançado, Emulador, Depuração) e a aba PS4; cada
  opção mostra o valor que vai rodar e quem decide. O arquivo de cada jogo
  guarda só o que você mudou.
- Diagnóstico: registros das últimas sessões (`startup.log`, `RPCS3.log`,
  `mesa.log`) e relatório de erro fatal na tela.

## Novidades da 0.40

- **Regra nova**: o que a 0.39 mudou para todos os jogos sem prova nos logs
  voltou ao jeito da 0.38 (cache de shaders do Mesa, "Áudio contínuo"
  automático e reserva de 64 ms, novas tentativas de criar threads, regras
  de memória e de compilação, idioma dos jogos e outras). Cada uma liga de
  novo com um arquivo em `/data/rpcs3` ou pelo `features.txt`, para testar.
  Continuam ligadas as correções provadas (como a do crash do intérprete de
  shaders) e as proteções que só agem quando algo dá errado.
- **Modo seguro**: um jogo que falha abre da próxima vez no modo seguro (3
  níveis, com aviso na tela). Também dá para escolher por jogo (Quadrado >
  "Modo seguro deste jogo") ou ligar para todos os jogos em Configurações do
  RPCS3 > PS4 > "Modo seguro".
- **Threads**: as últimas 8 vagas de thread do PS4 ficam para o jogo; as
  threads de serviço que não faziam nada não são mais criadas; a pausa e o
  savestate não derrubam mais o emulador quando falta thread; o som não
  deve mais sumir depois de uma pausa perto do limite.
- **Online e idioma**: Modo online sem conta RPCN abre o jogo offline; o
  "Automático" do "Idioma dos jogos" voltou a ser Inglês (EUA), e o idioma
  do PS4 virou uma opção da linha.
- **Gráficos**: o intérprete de shaders tem um limite de passos, a GPU
  travada volta mesmo para a XMB e um desenho pulado nunca conta como
  "escondido" (Sonic 06).
- **Configurações**: cada opção roda como você escolheu; só um limite real
  do PS4 ganha da sua escolha, com o cadeado e o motivo na página (ex.:
  "Escala de resolução" até 200 %). O `startup.log` diz quais valores rodam
  diferente dos seus arquivos e por quê.
- **Discos e ISOs sem internet**: a chave pode vir de um arquivo feito do
  seu próprio disco (`.dkey`, `.ird` ou lista de chaves) em
  `/data/rpcs3/keys` ou na pasta `keys` do pendrive, e cada chave que abriu
  um disco fica guardada no PS4. O pacote continua sem chaves. Para ler o
  leitor, o PS4 ainda precisa estar em uma rede (o FTP do GoldHEN).
- **XMB**: o fundo e a música do próprio jogo quando a seleção para nele
  (só os arquivos do jogo; o pacote não traz imagem nem música), pastas na
  coluna "Jogo" (Triângulo = Opções) e o segundo teclado seguido (RPCN,
  cheats) abre.
- **Ícone novo** do aplicativo.
- O primeiro boot de cada jogo na 0.40 monta o cache SPU de novo: não
  feche.

Quase tudo isso ainda precisa ser testado no console: o roteiro está em
"TESTE NO CONSOLE DA 0.40", no [`TESTAR_NO_PS4.txt`](TESTAR_NO_PS4.txt)
(mande os logs de cada teste).

## Novidades da 0.39

- **Crashes e travamentos (todos os jogos)**: corrigido o crash do intérprete
  de shaders depois de alguns minutos de jogo; a GPU emulada lê e grava as
  etiquetas de sincronização sem passar pela proteção da memória (a causa
  provável do congelamento do GTA IV); quando a GPU para de responder, o jogo
  fecha e volta para a XMB (antes ficava parado).
- **Threads**: threads que terminaram são liberadas, uma criação recusada pelo
  PS4 é tentada de novo e as threads de serviço só começam quando o jogo usa
  (jogos que ficavam sem som ou voltavam ao menu).
- **Áudio**: "Áudio contínuo" automático (o som fica um pouco mais lento,
  nunca abaixo de 70%, em vez de picotar) e reserva de 64 ms; os ajustes de
  áudio do menu do jogo valem na hora.
- **Discos e ISOs**: uma ISO nunca pede o disco no leitor (só a imagem que a
  linha "Disco de PS3" instalou); ISO criptografada abre sem chave manual (a
  chave é buscada sozinha); ISO incompleta é avisada na lista; ISO sem o
  cabeçalho de disco do PS3 abre; o disco tirado durante o jogo pausa o jogo.
- **FTP**: pastas criadas pelo FTP do GoldHEN ficam bloqueadas para o
  emulador; com o FTP ligado, o RPCS3 conserta isso sozinho ao abrir.
- **Vídeo**: cache de shaders do driver Mesa ligado por padrão (a segunda
  abertura de um jogo compila bem menos) e OpenGL mais leve.
- **Novo**: os idiomas da XMB, as Configurações do RPCS3 completas e os
  cheats (veja Recursos); os ajustes por jogo do RPCS3 de PC
  (`config_database.dat`) vêm dentro do pacote.

Os detalhes, as versões anteriores e o roteiro de testes no console estão em
[`TESTAR_NO_PS4.txt`](TESTAR_NO_PS4.txt).

## Requisitos

Console:

- PS4 ou PS4 Pro desbloqueado com **GoldHEN** (no PS4 Pro, ative o Modo
  Turbo/Boost Mode nas configurações do sistema);
- firmware oficial do PS3 (`PS3UPDAT.PUP`), instalado pela XMB (veja abaixo;
  ele fica em `/data/rpcs3/dev_flash`);
- os seus jogos de PS3 (pasta de disco, ISO ou PKG).

A CPU Jaguar do PS4 é bem mais fraca que a de um PC: jogos leves rodam bem,
jogos pesados podem ficar com FPS baixo. A primeira abertura de cada jogo
compila o código dele ("Compiling PPU Modules" / "Building SPU Cache") e pode
levar muitos minutos; as próximas usam o cache.

## Instalação

1. Copie o `.pkg` para `/data/pkg` e instale pelo GoldHEN
   (Debug Settings > Package Installer). Atualizar o pacote mantém as
   configurações e os caches.
2. Com o `PS3UPDAT.PUP` no pendrive (`PS3/UPDATE/` ou raiz), use XMB > Configurações > Instalar firmware do PS3.
3. Coloque os jogos em uma destas pastas:
   - `/data/rpcs3/games/NOME` (`PS3_GAME/USRDIR/EBOOT.BIN`) ou ISO em
     `/data/rpcs3/games`;
   - PKG de PS3 em `/data/rpcs3/pkgs`;
   - no pendrive: `PS3`, `GAMES`, `PS3ISO` ou `PKG`;
   - licenças `.rap` em `/data/rpcs3/raps` (ou junto dos PKG);
   - ou coloque o disco de PS3 no leitor do PS4 (com o FTP do GoldHEN
     ligado) e escolha "Disco de PS3" na lista para instalá-lo.
4. Abra o **RPCS3 PORT FOR PS4** na tela inicial.

O guia completo de uso (controles, menus, opções avançadas, registros) está em
[`TUTORIAL.txt`](TUTORIAL.txt) e [`TESTAR_NO_PS4.txt`](TESTAR_NO_PS4.txt), que também vão dentro do pacote.

## Como compilar

O build roda em um host Windows e gera o ELF nativo, o EBOOT e o PKG.

### Ferramentas

- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
  (inclui `create-fself`, `create-gp4` e `PkgTool.Core`, que precisa do .NET);
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 20240619 (UCRT,
  x86_64), usado como compilador cruzado (Clang/LLD) e para os testes de host;
- CMake 3.28 ou mais novo e Ninja (`build-llvm.ps1` usa
  `ps4-port/.build-tools/ninja.exe`);
- Python 3.12 ou mais novo, Git e Git for Windows (o bash dele compila o
  FFmpeg);
- Windows PowerShell 5.1 ou PowerShell 7.

### Código-fonte

Este repositório (com os submódulos do RPCS3) e, na mesma pasta pai:

```
<pasta>/
  rpcs3-ps4/            este repositório
  orbis-compat/         runtime de compatibilidade POSIX/libkernel
  orbis-porting-kit/    carregador Vulkan (vkloader)
  orbis-mesa-bundle/orbis-mesa-4c934ebf9cda/   build do Mesa (RADV, EGL, Gallium/Zink)
```

Os caminhos podem ser trocados com `RPCS3_PS4_ORBIS_COMPAT`,
`RPCS3_PS4_PORTING_KIT` e `RPCS3_PS4_MESA_BUNDLE`.

### Passos

Na raiz do repositório, em PowerShell (ajuste `$Sdk` e `$Llvm` para o seu
ambiente; os scripts aceitam `-Sdk` e `-LlvmTools`/`-CompilerBin`, e os
valores padrão apontam para pastas vizinhas ao checkout):

```powershell
$Sdk  = '<caminho>/OpenOrbis/PS4Toolchain'
$Llvm = '<caminho>/llvm-mingw-20240619-ucrt-x86_64/bin'

git submodule update --init --recursive

# Dependências fixadas: Abseil, GNU Make (MSYS) e LLVM para o console
./ps4-port/prepare-abseil.ps1
./ps4-port/prepare-make.ps1
./ps4-port/build-llvm.ps1 -Sdk $Sdk -CompilerBin $Llvm -Jobs 4

# Configuração (a pasta de build deve ser ps4-port/.build-abi)
cmake -S . -B ps4-port/.build-abi -G Ninja `
  -DRPCS3_PS4_PORT=ON `
  -DCMAKE_TOOLCHAIN_FILE=ps4-port/toolchain.cmake `
  "-DRPCS3_PS4_SDK=$Sdk" "-DRPCS3_PS4_LLVM_BIN=$Llvm" `
  -DRPCS3_PS4_WITH_LLVM=ON

# ELF nativo (o FFmpeg e o curl são compilados automaticamente)
cmake --build ps4-port/.build-abi --target rpcs3_ps4_elf -j 4

# EBOOT + PKG validados
./ps4-port/package-native.ps1 -Sdk $Sdk -LlvmTools $Llvm
```

`package-native.ps1` confere a versão em `rpcs3/util/ps4_build_version.hpp`,
valida o ELF (`verify-native-dispatch.py` e a política de saída do
processo), monta o PKG, extrai o conteúdo de volta e compara os hashes de
cada arquivo. O resultado fica em `ps4-port/dist/`:
`RPCS3_PS4_NATIVE_<versão>_<data>.pkg`, o `.sha256`, o `build-manifest` e
os guias `TUTORIAL.txt` e `TESTAR_NO_PS4.txt`.

Opcional: `-ConfigDatabase <caminho>/GuiConfigs/config_database.dat` coloca o
banco de ajustes por jogo do RPCS3 de PC no pacote, como
`/app0/config_database.dat` (o campo `config_database` do `build-manifest`
mostra o `timestamp` dele, ou `none`). Vem desligado: use só depois de
confirmar que a licença do arquivo permite distribuí-lo
(`rpcs3/util/ps4_config_db.hpp`).

Os certificados raiz do download de patches são
`ps4-port/assets/certs/cacert.pem` (cópia de
`3rdparty/wolfssl/wolfssl/wolfcrypt/src/port/Espressif/esp_crt_bundle/cacrt_all.pem`:
148 raízes da Mozilla, dados de 2024-07-02), instalados em
`/app0/certs/cacert.pem`.

### Testes de host

`ps4-port/tests/` contém testes que rodam no PC (Windows) com as chamadas do
kernel do PS4 simuladas: memória virtual e compartilhada, heap de memória
direta do orbis-compat, fenv, eventos atômicos, política de configuração,
vagas de controles, conta RPCN, XMB, USB/MIDI, áudio e outros. A linha de
compilação de cada teste está no cabeçalho do arquivo.

## Estrutura do projeto

| Caminho | Conteúdo |
| --- | --- |
| `ps4-port/CMakeLists.txt`, `*.cmake` | build cruzado: toolchain, dependências, LLVM, link nativo |
| `ps4-port/*.ps1`, `ps4-port/*.py` | preparação das dependências, build do LLVM e do FFmpeg, verificação do ELF e empacotamento |
| `ps4-port/runtime/` | implementações para o PS4: memória virtual e compartilhada, fenv, guarda de `exit()`, USB (sceUsbd), MIDI, captura de áudio, semente do wolfSSL |
| `ps4-port/compat/` | cabeçalhos de compatibilidade com o SDK OpenOrbis |
| `ps4-port/tests/` | testes de host e seus mocks |
| `ps4-port/tools/` | ferramentas de apoio (símbolos do profiler, busca de strings em binários) |
| `ps4-port/graphics/` | notas sobre a apresentação Vulkan/OpenGL |
| `ps4-port/probe/` | programa mínimo para testar o toolchain no console |
| `ps4-port/assets/` | ícone do aplicativo e certificados raiz (`certs/cacert.pem`) |
| `rpcs3/ps4_*.cpp` | frontend do console: entrada, launcher XMB, controles, teclado da tela, erro fatal, profiler |
| `rpcs3/util/ps4_*.hpp` | política de configuração, memória, XMB, RPCN, saúde do sistema e utilitários do port |
| `rpcs3/Emu/Audio/PS4/` | backend de áudio (AudioOut) |
| `rpcs3/Emu/NP/ps4_rpcn_service.*` | serviço RPCN do console |
| `rpcs3/Emu/NP/ps4_tls_selftest.*` | teste TLS do início (libcurl + wolfSSL por loopback, sem internet) |
| `ps4-port/curl.cmake`, `ps4-port/compat/ps4_curl_socket.h` | build do libcurl para o PS4 (sem `eventfd`, sockets sem bloqueio por `SO_NBIO`, sem IPv6) |
| `rpcs3/Input/ps4_pad_handler.*` | controles DualShock 4 (libScePad) |

## Créditos e licenças

**RPCS3** - todo o núcleo de emulação, renderizadores, recompiladores e
ícones da interface são do projeto RPCS3, da equipe RPCS3 e seus
colaboradores (https://rpcs3.net, https://github.com/RPCS3/rpcs3).
Licença **GPL-2.0** (`LICENSE` na raiz, também incluído no PKG). Este port é
distribuído sob a mesma licença.

Port para PS4: **Roberix Studio & Mifi Studios**.

Componentes de terceiros usados no build do console:

| Componente | Uso | Licença |
| --- | --- | --- |
| [LLVM](https://llvm.org) | recompiladores PPU/SPU | Apache-2.0 WITH LLVM-exception (`licenses/LLVM.txt` no PKG) |
| [Mesa](https://mesa3d.org) (RADV, EGL, Gallium/Zink) | Vulkan e OpenGL | MIT |
| [FFmpeg](https://ffmpeg.org) n8.1.1 | decodificação de áudio/vídeo | LGPL-2.1-or-later (compilado sem `--enable-gpl`/`--enable-nonfree`) |
| [wolfSSL](https://www.wolfssl.com) | TLS (RPCN, curl) | GPL-3.0, usado como GPL-2.0 pela exceção do wolfSSL para o RPCS3 |
| [curl](https://curl.se) | HTTP | licença curl (estilo MIT) |
| Certificados raiz da Mozilla (`certs/cacert.pem`, do `cacrt_all.pem` do wolfSSL) | verificação HTTPS do download de patches | MPL-2.0 |
| [miniupnpc](https://miniupnp.tuxfamily.org) | UPnP | BSD-3-Clause |
| [Protocol Buffers](https://protobuf.dev) | protocolo do RPCN | BSD-3-Clause |
| [Abseil](https://abseil.io) | dependência do Protobuf | Apache-2.0 |
| [zlib](https://zlib.net) | compressão (do pacote Mesa) | Zlib |
| [zstd](https://github.com/facebook/zstd) | compressão | BSD-3-Clause (ou GPL-2.0) |
| [libpng](http://www.libpng.org) | PNG | libpng-2.0 |
| [AsmJit](https://asmjit.com) | SPU ASMJIT | Zlib |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | configurações | MIT |
| [pugixml](https://pugixml.org) | XML | MIT |
| [SoundTouch](https://www.surina.net/soundtouch) | áudio | LGPL-2.1 |
| [Fusion](https://github.com/xioTechnologies/Fusion) | sensores de movimento | MIT |
| [glslang](https://github.com/KhronosGroup/glslang) | SPIR-V | BSD-3-Clause e outras (ver `LICENSE.txt` do glslang) |
| [Vulkan Memory Allocator](https://gpuopen.com/vulkan-memory-allocator) | Vulkan | MIT |
| Cabeçalhos Vulkan (Khronos) | Vulkan | Apache-2.0 OR MIT |
| [stb](https://github.com/nothings/stb) | imagens e fontes | MIT ou domínio público |
| [libusb](https://libusb.info) (cabeçalhos) | API USB implementada sobre sceUsbd | LGPL-2.1 |
| [RtMidi](https://github.com/thestk/rtmidi) (API C) | MIDI USB | licença RtMidi (estilo MIT) |
| [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) | SDK e ferramentas de empacotamento; runtime musl (MIT) e libc++/libc++abi/libunwind (Apache-2.0 WITH LLVM-exception) | GPL-3.0 (ferramentas) |
| orbis-compat | runtime de compatibilidade POSIX/libkernel | MIT (exceções indicadas por arquivo) |
| [orbis-porting-kit](https://github.com/orbis-ports/orbis-porting-kit) | carregador Vulkan (vkloader) | MIT (exceções indicadas por arquivo) |
| [DejaVu Fonts](https://dejavu-fonts.github.io) | fontes do launcher | licença Bitstream Vera / DejaVu (alterações em domínio público) |

As licenças completas estão nos diretórios de cada componente em `3rdparty/`
e nos repositórios citados. PlayStation, PS3 e PS4 são marcas da Sony
Interactive Entertainment.
