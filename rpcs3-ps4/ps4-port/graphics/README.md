# Apresentação gráfica no PS4

## Vulkan

O alvo `rpcs3_ps4_vulkan` compila todos os arquivos Vulkan que o CMake do
RPCS3 lista, ou seja, o `VKGSRender` original. O frontend compila com
`HAVE_VULKAN=1`. O `ps4-vkloader` (do orbis-porting-kit) fornece os símbolos
Vulkan estáticos e liga o driver RADV e o zlib do pacote Mesa PS4. Os
cabeçalhos Vulkan vêm do mesmo pacote.

No PS4 a superfície é criada com `VK_EXT_headless_surface`; o Mesa PS4
apresenta a swapchain headless pelo `sceVideoOut`. O
[exemplo triangle do orbis-porting-kit](https://github.com/orbis-ports/orbis-porting-kit/tree/main/examples/triangle)
mostra esse caminho de apresentação de forma isolada.

O RADV é ligado inteiro (`WHOLE_ARCHIVE`), porque as tabelas de dispatch do
Mesa referenciam muitos handlers apenas de forma fraca.
`verify-native-dispatch.py` confere no ELF final que os handlers obrigatórios
existem e que a tabela de dispatch aponta para eles.

## OpenGL

O `GLGSRender` original usa o Mesa EGL com Zink sobre o mesmo RADV. O Mesa
substitui apenas o contexto/janela da plataforma e o carregador de funções.
`prepare-mesa-gl.py` remove dos arquivos EGL e Gallium os objetos idênticos
aos que o RADV já contém, mantendo os erros de símbolo duplicado para
qualquer outro conflito.
