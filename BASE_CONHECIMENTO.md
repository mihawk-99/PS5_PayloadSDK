# Base de conhecimento — PS5 Payload SDK

Este documento resume a estrutura e os caminhos de uso encontrados neste repositório. A referência principal para as instruções de instalação continua sendo [README.md](README.md); para detalhes exclusivos da camada `platform/`, veja [platform/README.md](platform/README.md).

## Visão geral

Este fork do `ps5-payload-dev/sdk` fornece ferramentas e bibliotecas para compilar payloads ELF para PS5s com ambiente de homebrew habilitado. O README registra o estado do fork como mesclado até o upstream em 2026-11-06, após v0.45. O fork acrescenta principalmente `platform/`, ajusta wrappers de compilador em `host/bin/` e corrige o contexto de máquina em `include/freebsd/sys/_ucontext.h`.

O SDK não é um SDK oficial Sony nem uma abstração de alto nível para jogos. É uma base de runtime/toolchain: compila C/C++, empacota símbolos/imports de bibliotecas do sistema e oferece componentes de compatibilidade e acesso a recursos de baixo nível.

## Mapa do repositório

| Diretório | Papel |
| --- | --- |
| `crt/` | Runtime de inicialização e carregamento: payload, ELF/SO, SPRX, resolução de NID, syscall e suporte de depuração. |
| `include/` | Headers de sistema e compatibilidade. Inclui headers FreeBSD, Khronos (EGL/GLES) e headers públicos `include/ps5/` para payload, kernel, IOMMU, NID e logging. |
| `libc/` | Implementações/compatibilidade da libc usadas no SDK, em grande parte derivadas de FreeBSD. |
| `libufs/` | Leitura e suporte a estruturas UFS. |
| `sce_stubs/` | Stubs de ligação para bibliotecas `libSce*` e kernel. Resolvem referências durante a linkedição; não são implementações funcionais dessas APIs. `genstub.py` gera stubs de SPRX disponíveis localmente. |
| `host/` | Ferramentas de host e wrappers do compilador/linker, incluindo ferramentas Windows em `host/win/`. |
| `platform/` | Camada adicional deste fork: `libps5platform.a`, headers e testes host para funcionalidades/limitações específicas observadas no console. |
| `samples/` | Exemplos independentes, cada qual com seu Makefile: hello world/argv/stdio, módulos e SPRX, linking relocatable, HTTP/2, notificações, informações do sistema, arquivos, privilégios e projetos CMake/Meson/C++. |

## Abstrações principais

### Runtime e bibliotecas do sistema

`crt/` contém o código que inicia um payload e resolve dependências/imports ELF e SPRX em runtime. NIDs são identificadores usados para localizar exports de funções do sistema. `sce_stubs/` fornece símbolos para o linker; ter um símbolo no stub não prova que a chamada seja permitida ou que funcione em runtime. A disponibilidade depende do export real, firmware, permissões e tipo de processo.

Para usar uma biblioteca SCE adicional, o README descreve fornecer um SPRX descriptografado, criar o link esperado em `sce_stubs/`, gerar os stubs e instalar o SDK novamente. O stub é uma ponte de linkedição, não um wrapper documentado nem código da biblioteca.

### Camada `platform/` (específica deste fork)

O projeto compila `platform/src/` em `libps5platform.a` e instala seus headers em `include/ps5platform/`. Os grupos de API observados incluem:

- **Heap em memória direta:** `heap.h` / `heap.c`, baseado em mspaces de dlmalloc, com arenas por thread (até oito); opcionalmente substitui a família `malloc` por linker `--wrap` ou pode coexistir com outro allocator.
- **Memória executável e compartilhada:** `exec.h` e `shm.h` administram código executável em direct memory, objetos compartilhados com múltiplas views e reservas de intervalos virtuais.
- **Compatibilidade libc/POSIX:** `libc.h` agrega substituições para funções ausentes, recusadas ou defeituosas no ambiente title, incluindo famílias de diretórios e `*at`, locale, regex, memória de stream e outras descritas no README.
- **Kernel, contexto e estado da CPU:** `kernel.h`, `context.h` e `fp.h` centralizam declarações de exports, layout do contexto e estado de ponto flutuante. A correção de `mc_rip` no `ucontext` é relevante para identificar a instrução que causou uma falha.
- **Logging e GPU/saída de vídeo:** `klog.h`, `agc.h` e `videoout.h` expõem declarações usadas pela camada e por consumidores.
- **Diagnóstico de capacidades:** `probe.h` e `platform/docs/PROBE.md` documentam probes executados no console, com evidências em `platform/evidence/`.
- **Acesso elevado e recursos auxiliares:** `elevation.h` pede ao daemon Lapy acesso a caminhos como `/data`; outros headers cobrem offload, FTP e descrição de arquivos, conforme cada interface.

A camada busca chamar funções exportadas e basear afirmações sobre o console em medições/probes próprios. Os testes em `platform/tests/` usam um modelo host do kernel; isso valida lógica local, não substitui validação no console.

## Fluxo de desenvolvimento

1. Instale os pré-requisitos do host listados no README (Clang/LLVM e LLD; dependências extras são necessárias para alguns exemplos e geração de stubs).
2. Instale a distribuição do SDK ou compile/instale o repositório. Defina `PS5_PAYLOAD_SDK` apontando para a raiz instalada.
3. Compile um exemplo com `make -C samples/<exemplo>`; o `hello_world` é a introdução mínima.
4. Configure `PS5_HOST` e `PS5_PORT` conforme o loader escolhido e use o alvo `test` do exemplo para enviar/executar no console.
5. Inclua bibliotecas e stubs necessários na linkedição; para código que usa `platform/`, vincule `libps5platform.a` e use os headers `ps5platform/`.

Os detalhes de instalação e dependências variam por sistema operacional; siga os comandos atuais do README em vez de assumir caminhos ou versões fixos.

## Microfone do controle — achados e implementação

**Destaque:** há evidência empírica de captura pelo serviço `libSceAudioIn`, que lê a entrada selecionada pelo sistema para um usuário. Isso não equivale a ler os pacotes HID/Bluetooth do DualSense; se o usuário selecionou headset ou USB como entrada, é essa rota que o SDK pode receber. O mute do controle e as configurações do PS5 afetam a entrada.

O código acessível mais concreto é [`PS5_RetroArch/src/microphone_ps5.cpp`](https://github.com/mihawk-99/PS5_RetroArch/blob/main/src/microphone_ps5.cpp), cujos comentários reportam validação de captura nativa e mute no firmware 13.60. Ele inicializa AudioIn, abre porta para o usuário, lê blocos PCM S16 mono numa thread e os oferece por uma fila limitada. Tenta tipos 0/1, 48 kHz com fallback a 16 kHz e blocos de 256 frames. Um relatório independente diz que tipos 0, 1 e 5 produziram PCM válido em duas execuções no firmware 6.02; por isso o wrapper tenta os três nessa ordem ([relatório da POC](https://github.com/blackbearreloaded/ps5-audio-decoding-research/blob/main/examples/native-audio-poc/README.md)). A interpretação `(silent_state & 5)` como ausência de dispositivo ou mute é observada no driver, não contrato oficial. Uma sonda independente lista as mesmas funções e experimenta formatos/tamanhos: [Halo Android AudioIn probe](https://github.com/zay448345045/halo-android/blob/main/port/ps5/probe/audioin/main.c).

Este fork agora fornece a API em `platform/include/ps5platform/microphone.h`, implementação em `platform/src/microphone.c`, declarações empíricas em `include/ps5/audioin.h`, import stub `sce_stubs/libSceAudioIn.c` e exemplo em `samples/microphone_stream/`. O wrapper mantém PCM mono assinado de 16 bits numa fila limitada, descarta áudio antigo se a fila lotar, oferece leitura com timeout e estatísticas; o chamador escolhe explicitamente o user id. Também limpa buffers da fila ao entrar em mute/fechar a stream e aceita códigos observados de AudioIn já inicializado. O exemplo captura por 30 segundos, mostra nível e estado de silêncio e inclui comandos de compilação/deploy no README. Compile o consumidor com `-lps5platform -lSceAudioIn`. O instalador `platform/tools/setup-sdk.sh` constrói e inclui o stub no SDK derivado do release upstream.

O exemplo calcula métricas do stream ao vivo e não grava voz em arquivo. Consulte [DUALSENSE_MICROPHONE.md](platform/docs/DUALSENSE_MICROPHONE.md) para uso e ressalvas. As assinaturas e valores de porta/formato continuam ABI empírica; não há validação deste novo wrapper em console nesta etapa. Uma pesquisa mais recente indexou o README da POC 6.02 com os resultados reportados; não consegui buscar os arquivos-fonte da POC diretamente. A fonte de referência 13.60 é o driver RetroArch. A documentação oficial da [Sony](https://www.playstation.com/en-us/support/hardware/ps5-change-audio-output/) cobre seleção e mute da entrada, não a API de programação.

## Limitações e como interpretar evidências

- Um arquivo em `sce_stubs/` comprova apenas que há um símbolo de ligação listado; não comprova que o sistema exporte a função em todo firmware nem que ela seja acessível ao processo.
- Headers copiados de FreeBSD/Khronos oferecem tipos e APIs compatíveis, mas não garantem que o kernel/firmware do PS5 implemente cada chamada.
- `platform/docs/PROBE.md` e `platform/evidence/` são evidência empírica por firmware/configuração; não generalize além do que foi medido.
- O fork contém componentes upstream e componentes locais. `README.md`, `platform/README.md` e `UPSTREAM_CONTRIBUTIONS.md` esclarecem essa fronteira e decisões de manutenção.

## Pontos de entrada para exploração

- Comece por `README.md` para instalação, build, execução e adição de stubs.
- Leia `samples/hello_world/` e depois os exemplos que correspondem à necessidade (por exemplo `hello_dlfcn`, `hello_sprx`, `relocatable_linking`, `http2_get`).
- Para APIs locais deste fork, consulte os headers `platform/include/ps5platform/` e a implementação correspondente em `platform/src/`.
- Para comportamento observado no console, consulte `platform/docs/PROBE.md` e os relatórios de `platform/evidence/`.
- Para symbols de áudio/controller, examine `sce_stubs/libScePad.c`, `sce_stubs/libSceUserService.c`, `sce_stubs/libSceAudioOut.c` e a lista de módulos em `crt/rtld_sprx.c`, mantendo a distinção entre stub e implementação.

## Análise aprofundada por subsistema

### Toolchain, ABI e linkedição

O alvo usado pelos wrappers é `x86_64-sie-ps5`: CPU x86-64 e ABI/ambiente Prospero. `host/toolchain/` oferece configurações para Make, CMake, Meson e shell; `host/elf_x86_64.x` define os segmentos ELF e as seções de inicialização/finalização. Os wrappers `prospero-clang` e `prospero-clang++` selecionam sysroot, headers e bibliotecas instaladas, incluem por padrão o CRT, libc e stubs de `libkernel_web`, `libSceLibcInternal` e `libSceNet`, e encaminham a linkedição ao LLD. Opções como `-nostdlibc`, `-nodefaultlibs`, `-nostartfiles` ou especificar outra variante de kernel alteram esses defaults.

O wrapper local de Clang força matemática IEEE para denormais, porque o alvo de fábrica assume flush-to-zero e isso gerou resultados numéricos incorretos nos consumidores documentados pelo fork. O projeto usa PIC e TLS emulado em vários componentes. O destino típico de instalação é `/opt/ps5-payload-sdk`, com artefatos sob `target/`; projetos consumidores importam `toolchain/prospero.mk` ou o toolchain de seu build system.

### Inicialização, syscalls e loader

`crt/_start.S` é a porta de entrada. O runtime em `crt/crt.c` recebe `payload_args_t`, inicializa a ponte de syscalls e os exports do kernel, prepara logging/patches/loader, localiza `main(argc, argv, envp)`, executa construtores/destrutores e permite terminar o payload com `payload_exit`. Os argumentos incluem a função `sys_dynlib_dlsym` e endereços/descritores fornecidos pelo loader externo; portanto, o modelo de execução depende de como o ELF foi injetado/carregado.

`crt/syscall.c` encapsula a interface de syscall fornecida nos argumentos do payload. `crt/kernel.c` resolve funções de kernel/exportações e encapsula operações como leitura/escrita de memória de processo, permissões de VM, localização de processos/descritores, resolução de símbolos em módulos e leitura/ajuste de campos de credenciais. `kernel_iommu.c` expõe escrita por comandos IOMMU. `kernel_procio.c` complementa acesso a processo. Os headers `include/ps5/kernel.h` e `include/ps5/iommu.h` tornam essas operações acessíveis a consumidores; são interfaces de baixo nível e dependentes de contexto/firmware, não uma camada portátil de aplicação.

O loader mantém uma árvore de dependências (`rtld_lib_t`) e abstrai cada módulo por callbacks comuns (open/init/symbol lookup/fini/close). `rtld_payload.c` trata o ELF principal; `rtld_so.c` mapeia objetos ELF `.so`, processa segmentos dinâmicos, dependências e relocações x86-64; `rtld_sprx.c` delega ao sistema PS5 para carregar módulos `.sprx`, usando handles e uma tabela de módulos de sistema (`sceSysmoduleLoadModuleInternal`). `rtld.c` resolve caminhos de sistema/homebrew/`LD_LIBRARY_PATH`, evita duplicar carregamentos e mantém contagens de referência. `rtld_dlfcn.c` dá suporte à família `dlopen`/`dlsym`/`dlclose` e `dladdr`. `crt/nid.c` codifica nome de export SCE em NID; os stubs e o runtime usam nomes/NIDs para conectar imports.

Não confundir os formatos: `.so` é mapeado e relocacionado pelo loader do SDK; `.sprx` é carregado pelo mecanismo do sistema. A disponibilidade de um SPRX na tabela do loader e de um símbolo em um stub só permite tentar a resolução. Pode ainda falhar por caminho, versão, permissão, export ausente ou restrição do processo. O próprio Makefile de stubs contém TODO sobre dependências `DT_NEEDED`, sinal de que não se deve presumir um grafo de dependências completo para todo SPRX adicional.

### Headers de compatibilidade e bibliotecas de suporte

`include/freebsd/` fornece headers de compatibilidade com a base FreeBSD, incluindo interfaces POSIX/BSD e headers de sistema. `include/khronos/` fornece EGL e GLES. `include/ps5/` contém as APIs do projeto/runtime: `payload.h`, `kernel.h`, `iommu.h`, `nid.h`, `klog.h` e `mdbg.h`. A inclusão de um header não significa que todos os símbolos tenham implementação no sistema; verifique biblioteca, stub, export real e firmware.

`libc/` complementa a libc do target com funções FreeBSD/POSIX que faltam ou foram adaptadas. Os grupos são: resolução de símbolos e carregamento (`dlfcn`); parsing de rede (`netdb`, interface names), utilitários de filesystem (`fts`, `nftw`, `scandir`, `mount`, `getmntinfo`); regex TRE (`regcomp/regexec/regfree/regerror`); threads e TLS/C11 (`mtx`, `thrd`, `tss`, `cnd`, `call_once`, `emutls`); tempo, locale e caracteres (`gmtime`, `localtime`, `asctime_r`, `ctime_r`, `nl_langinfo`, conversões multibyte); sinais e utilitários (`killpg`, `backtrace`, `syslog`, `popen`, `tmpfile`, `pipe2`, `recvmmsg/sendmmsg`, `mman`, `termios`, `getloadavg`) e famílias de busca/strings. Os fontes são compilados para o alvo Prospero com os headers deste repositório. O Makefile monta `libc.a`; `libdl.a` e `libpthread.a` são arquivos vazios, úteis para compatibilidade de linha de linkedição enquanto símbolos reais vêm de outras bibliotecas/stubs.

`libufs/` implementa leitura/manipulação de estruturas UFS (superbloco, grupos de cilindros, inode e blocos) por descritor de dispositivo e é componente especializado, não a camada geral de sistema de arquivos. `sce_stubs/` gera `.so` de ligação a partir dos símbolos compilados; quando existe um SPRX local e a base de NIDs, `genstub.py` pode materializar o C correspondente.

### Inventário da camada `platform/`

Além dos grupos já descritos acima, a leitura das interfaces e fontes identificou:

| Componente | Arquivos-chave | Abstração e uso |
| --- | --- | --- |
| Alocação direta/posicionamento | `kernel.h`, `placement.c`, `exec.c`, `shm.c` | Obtém direct memory via exports de kernel, reserva endereços fora da janela da GPU ou perto de um anchor, faz mapeamentos e libera recursos em caminhos de erro. Granularidades medidas: páginas de 16 KiB e unidades de direct memory de 64 KiB. |
| JIT | `exec.h`, `exec.c` | Cria regiões executáveis com posicionamento livre, fixo, perto do código, endereço candidato sem substituir mapeamento, dual view RW/RX ou alternância de proteção. Mantém contadores e tenta limpar alocação parcial ao falhar. |
| Shared memory e virtual ranges | `shm.h`, `shm.c` | Cria backing compartilhado, mapeia views com offset/proteção, reserva regiões e faz commit/decommit em unidades, com contabilidade de objetos/views/ranges. |
| Heap | `heap.h`, `heap.c`, `heap_wrap.c`, `src/dlmalloc/` | Arenas mspace em segmentos de direct memory, arena associada à thread, estatísticas, trim e hooks de rastreamento. Wrappers preservam ponteiros de libc com fallback para o allocator original; o título pode consumir as funções explícitas se tiver allocator próprio. |
| Threads e estado FP | `threads.c`, `fp.c`, `context.h` | Wrappers de criação/join/detach, stacks em direct memory com reciclagem/reaper, execução de destructors TLS e estado MXCSR IEEE. Testes/probe tratam afinidade e comportamento do scheduler. |
| libc e sistema de arquivos | `libc.c`, `directory.c`, `posix.c`, `locale.c`, `xlocale.c`, `regex.c`, `regex_abi.c`, `memstream.c`, `cxa.c` | Funções `ps5_*` implementam as ausências/peculiaridades do target sem se apropriar automaticamente dos nomes libc; os wrappers especiais cuidam de `fclose`/`fflush` de memstream e destructors TLS. Regex adapta TRE de musl à ABI FreeBSD e usa asserts de layout/constantes. |
| Virtualização de descritores (FDV) | `fdv.h`, `fdv.c` | Mitiga o limite medido de descritores de arquivos abertos por caminho: estaciona arquivos regulares ociosos substituindo temporariamente descritores por sockets, reabre e restaura posição quando usados. O consumidor deve envolver chamadas que usam FDs com `enter/leave`, acompanhar dup/rename/SCM_RIGHTS e marcar FDs que não podem ser estacionados. Não é transparente se o app não integrar esses hooks. |
| Offload de gravação | `ftp.h`, `ftp.c`, `offload.h`, `offload.c` | Usa FTP local do console e conexão loopback para estender arquivos do próprio título quando a taxa local cai após o burst de escrita. Enfileira blocos, confirma a transferência, mantém o arquivo no mesmo inode e possui fallback de gravação local quando o servidor falha. Requer setup e raízes de armazenamento compatíveis. |
| Observabilidade | `klog.h`, `klog.c`, `report.c`, `memory.c`, `probe*.c` | Captura stderr no kernel log, produz relatório e snapshot de memória e executa probes de arquivos, threads, memória, JIT e comportamento de kernel. Os probes não são APIs de runtime gerais e podem ser executados como ferramenta de diagnóstico. |
| Elevação cooperativa | `elevation.h`, `elevation.c`, `docs/ELEVATION.md` | Cliente de protocolo para daemon Lapy em processo separado, com publicação atômica de pedido e prova real de acesso a `/data`. Este repositório não contém o daemon nem lógica de kernel; erro/timeout significa que o recurso não foi concedido. |
| AGC e saída de vídeo | `agc.h`, `videoout.h` | Declarações unificadas usadas por drivers gráficos de consumidores. Os comentários registram assinaturas e comportamento observados em execuções do projeto, não substituem documentação oficial. |

`platform/Makefile` tem dois produtos distintos: a biblioteca de console, compilada com target PS5, e testes host (modelo de kernel/libc). `make test` não executa código de console. O probe console (`probe.c`) não integra o conjunto host porque depende de exports e comportamento reais.

### Catálogo funcional dos exemplos

| Exemplo | O que demonstra |
| --- | --- |
| `hello_world`, `hello_argv`, `hello_stdio` | Entrada `main`, argumentos e I/O padrão. |
| `hello_cmake`, `hello_meson`, `hello_cxx`, `hello_cxx23` | Integração com build systems e dialetos de C++. |
| `hello_dlfcn` | `dlopen`/`dlsym` de SPRX, lookup no próprio payload e lookup de símbolo libc. |
| `hello_sprx` | Referência direta/linkedição contra `libSceRandom`. |
| `hello_so`, `relocatable_linking` | Dependências entre `.so`, SONAME/caminhos, linking relocatable e resolução do loader. |
| `arbitrary_syscall`, `hello_kenv`, `hwinfo`, `ps`, `mntinfo`, `list_files` | Syscall, kenv, informações do console, processos, mounts e filesystem. |
| `http2_get` | Inicialização das bibliotecas SCE Net/SSL/HTTP2 e request de rede. |
| `browser`, `notify`, `notify_debug` | UserService/SystemService para lançar browser e APIs de notificação (um exemplo também usa notificação de kernel). |
| `install_app` | Demonstra fluxo de empacotamento fake SELF/param/icon e instalação por APIs SCE; não é um tutorial de publicação oficial. |
| `test_privileges` | Inspeção/teste de permissões e interfaces de processo no contexto alvo. |

Os exemplos são provas de conceito para interfaces específicas e muitos declaram funções SCE manualmente em vez de incluir headers tipados. Use-os para entender o caminho de build e a forma das chamadas, não como contratos completos de API ou garantias de disponibilidade por firmware.

## Como navegar e validar uma funcionalidade

Para investigar uma chamada do console, siga esta cadeia: (1) procure símbolo no exemplo e no stub; (2) confirme qual `.sprx` o linker/runtime carrega; (3) identifique se o header tem assinatura e de onde veio; (4) siga a implementação do loader para entender se a resolução é dinâmica ou feita pelo sistema; (5) verifique permissões e pré-condições do processo; (6) procure probe/evidência com firmware; (7) só então trate como funcionalidade suportada. Esse método é especialmente importante para APIs de controller/áudio e para funções com nomes de kernel.

Para contribuições locais, identifique primeiro se o arquivo é upstream, dependência vendorizada ou parte do fork. `UPSTREAM_CONTRIBUTIONS.md` registra decisões sobre propostas ao upstream. `platform/tools/setup-sdk.sh` explica o modelo de consumo do fork: exporta uma revisão, baixa a release upstream esperada, verifica digest e sobrepõe headers/wrappers/camada platform, deixando binários upstream como publicados.

## Escopo desta análise

Esta revisão cobriu a estrutura completa, o README e documentos da camada platform, Makefiles principais, wrappers/toolchains, headers públicos PS5 e platform, organização/fluxos centrais de `crt/`, catálogo dos exemplos e busca direcionada em áudio/controller. `include/freebsd/`, `include/khronos/`, `libc/` e arquivos gerados de stubs contêm grande volume de código de base/vendorizado; foram classificados por árvore e pontos de integração, não reescritos API por API nesta base de conhecimento. Para uma auditoria de função específica, o caminho de navegação acima indica os arquivos primários.

## Build, validação e distribuição

`ci.sh` faz um build de instalação em diretório temporário, compila libc++ e constrói um subconjunto de amostras com Make, CMake e Meson. Os workflows GitHub Actions para Ubuntu e macOS também constroem SDK e subconjunto de exemplos e empacotam artefato; a variante Ubuntu ativa `WIN=1` para incluir wrappers Windows. Isso verifica compilação e integração de toolchain, não execução no PS5. Algumas amostras do catálogo (incluindo `hello_so`, `http2_get` e `test_privileges`) não aparecem na lista principal do CI, então a aprovação do CI não demonstra que todas compilaram naquele pipeline.

`libcxx.sh` busca LLVM 18.1.8 e configura libc++, libc++abi e libunwind estáticos, com exceções e RTTI habilitados. `platform/tools/setup-sdk.sh` serve a outro caminho: parte de uma revisão exportada, baixa a release upstream v0.45, verifica SHA-256, instala headers desta árvore e `libps5platform.a` sobre os binários upstream e grava a revisão em `.ps5-sdk-revision`. O script exige ferramenta POSIX (`flock`, `wget`, `unzip`, `sha256sum`, `make`) e pode substituir o diretório destino do SDK; consumidores devem apontá-lo apenas para o destino deliberado.

Ferramentas de host: `prospero-deploy` envia payload ELF por TCP para loader configurado; `prospero-nid` calcula o NID de nome de símbolo; `prospero-pkg-config` direciona busca de `.pc` para o sysroot/homebrew; `prospero-fetchpkg` obtém pacotes referidos por manifest JSON. A leitura de `prospero-fetchpkg` mostra que seu cliente HTTP desativa verificação de certificado/hostname e apenas avisa sobre hashes/tamanhos divergentes, sem abortar necessariamente. Isso é uma limitação concreta: não use como verificador seguro de integridade/transporte sem endurecer o script e confirmar artefatos por fonte confiável. O wrapper de deploy também aceita URI/string além de caminho local e escreve diretamente na conexão do loader.

## Superfície dos stubs SCE

O inventário contém stubs para kernel (`libkernel*`), libc interna, rede/SSL/HTTP, serviços de usuário/sistema, módulos, pad/keyboard/ime, áudio de entrada e saída, vídeo, GPU/AGC/GNM/EGL, instalação de app, notificações, remote play e alguns componentes adicionais (por exemplo Mono e filesystem interno). `libSceAudioIn.c` nesta branch é um import stub mínimo; não implementa a captura, que ocorre na biblioteca de sistema do console.

Os arquivos de stub são majoritariamente declarações de símbolos globais em assembly sem instrução de função: permitem gerar uma biblioteca compartilhada cujo símbolo poderá ser resolvido pelo carregador/console. `genstub.py` lê exports de um SPRX ELF e traduz NIDs via `aerolib.csv`, emitindo símbolos de função/objeto; símbolos desconhecidos são omitidos com warning. Assim, existem três níveis distintos: nome no stub, identificação/resolução pelo loader e função realmente exportada/permitida pelo sistema naquela execução. O stub sozinho não prova ABI, assinatura, semântica nem sucesso.

## Compatibilidade, dependências e licenças

- `include/freebsd/` é uma árvore extensa de headers FreeBSD; `libc/` também deriva/adapta componentes FreeBSD. A licença-raiz é GPLv3+, mas há exceções explícitas: arquivos de FreeBSD sob licenças BSD, TRE/regex de musl sob MIT e dlmalloc sob MIT. Para redistribuição, a licença deve ser avaliada por arquivo/componente, não apenas pela licença-raiz.
- `include/khronos/` traz interfaces EGL/GLES 2.0 e extensões Khronos. Isso fornece declarations, não driver/runtime gráfico por si só.
- `libufs/` é implementação userland para leitura/manipulação de UFS através de descritor de dispositivo (`struct uufsd`, superbloco, cylinder groups, inode e blocos). É independente da camada `platform/`; operar em dispositivos segue as permissões que o processo recebe.
- A distribuição upstream preserva binários já compilados (CRT, libc, stubs, libc++ e ferramentas). O build local recompila subárvores e os scripts CI testam o toolchain, portanto resultados binários podem diferir da release. A camada platform tem uma suíte host separada e probes de console específicos.
