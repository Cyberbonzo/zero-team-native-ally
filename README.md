# Zero Team USA no ROG Ally

Port Linux x86-64 do arcade da Seibu Kaihatsu (1993), feito a partir da edição
**Zero Team USA (US)**, conjunto `zeroteam`. O programa V30 do jogo é traduzido
estaticamente para x86-64 durante a compilação, e a placa usa os dispositivos
Seibu do FBNeo para memória, vídeo, som e entrada. O executável e a biblioteca
são específicos deste título; a compilação usa a ROM do próprio jogador.

## Jogar no Ally

O atalho **Zero Team USA** está na biblioteca da Steam. A instalação fica em
`~/Games/ZeroTeam-Native` (NVMe interno; o cartão microSD estava somente
leitura no dia da instalação). O jogo ocupa os 1920×1080 do Ally **sem
esticar**: o renderizador passou a ler 428 colunas de mundo (as 320 do gabinete
mais 54 de cada lado) e a frente estica essa imagem para a tela inteira.

| Ação | ROG Ally | Teclado |
|---|---|---|
| Inserir crédito | View | 5 |
| Iniciar | Menu / Start | 1 ou Enter |
| Andar | Direcional ou analógico esquerdo | Setas |
| Atacar | A | Z |
| Pular | B | X |
| Pausar | Y | Esc |

A lógica roda na cadência original do arcade (55,47 Hz) e a apresentação sai a
60 Hz, repetindo o quadro quando não há um novo. Assim a velocidade do jogo não
muda e a imagem não trava. O log em `logs/game.log` mostra, ao encerrar, a média
de quadros da lógica e da apresentação, além de quantos foram repetidos.

**Tela interna e saída externa.** A frente abre na saída externa quando existe
uma (HDMI/USB-C) e no painel do aparelho quando não existe; `--display N` força
uma tela específica e `--display auto` é o padrão. Em ambos os casos ela pede um
modo de 1920×1080 quando a tela oferece um, para a saída ser 1080p também fora
do aparelho. `--pillarbox` recorta as 320 colunas originais e mostra em 4:3,
com barras, para quem preferir o enquadramento exato do gabinete.

Este build não tem save-state: o jogo não grava progresso próprio, e o suporte a
estado salvo ainda não foi implementado nem verificado.

## Compilar com sua ROM

Em Linux x86-64, a compilação usa Git, Python 3, GCC/G++, Make, pkg-config e os
arquivos de desenvolvimento do SDL2 e zlib.

```sh
bash build.sh "/caminho/para/Zero-Team-U-S-A_Arcade_EN.zip"
```

O script aceita o ZIP externo ou o `zeroteam.zip` interno, confere os 13 chips
por tamanho e CRC, obtém a revisão fixada do FBNeo, gera a tradução, compila a
placa traduzida **e** a de referência, prova que as duas produzem os mesmos
quadros e áudio por 1.200 quadros, e monta a distribuição em `.work/dist/`.
Execute `jogar.sh` ou adicione esse arquivo à Steam.

## Como funciona

O gerador lê os quatro chips de programa (256 KiB cada, intercalados de 4 em 4)
e monta a imagem de 1 MiB que o driver mapeia em `0x20000–0xfffff`. Para **cada
endereço de byte** dessa imagem ele emite uma entrada nativa de 8 bytes que
desvia para a implementação já compilada do opcode guardado naquele endereço:
917.504 endereços, 256 opcodes distintos, 7,34 MB de código gerado. O despacho
do núcleo NEC do FBNeo (`NEC_OPTABLE[fetchop(...)]`) é substituído por essa
entrada, então a decodificação "endereço → opcode → rotina" acontece uma vez, na
compilação.

O resto da placa vem do FBNeo na revisão fixada: mapa de memória e temporização
do driver, dispositivo Seibu COP (SEI252), decodificação e desenho dos sprites,
tilemaps, YM2151 + 2× MSM6295 e o Z80 de som. A interface SDL2 apresenta o
resultado e lê os controles.

O gerador e as ferramentas estão neste repositório. As imagens dos chips, o
código gerado a partir delas e a biblioteca resultante ficam na cópia privada do
jogador. As condições de uso do código do FBNeo estão em
[licenses/FBNeo.txt](licenses/FBNeo.txt). [Fontes dos dispositivos na revisão
usada](https://github.com/finalburnneo/FBNeo/tree/61cd3ede225ec44b84011f7679db68123d72725d).

## Campo de visão 16:9

A placa desenha os tilemaps e os sprites de um mundo de 512 px e recortava tudo
em 320 colunas. Para o campo 16:9 o recorte passou a 428 colunas e as camadas
receberam um deslocamento de 54 px, então a janela mostra 54 px de mundo a mais
de cada lado. A camada de texto fixa (placar, tempo, avisos) continua com as suas
40 colunas centradas, e nada do miolo muda: o centro de 320 colunas da imagem
16:9 é **byte a byte idêntico** ao quadro 4:3 do mesmo instante.

**Limite conhecido.** O mundo tem 512 px e o jogo escreve apenas a janela que o
gabinete mostrava. No começo de cada fase, e nas telas fixas (título, seleção de
personagem), as colunas extras podem cair fora da área desenhada do mapa e
aparecer escuras; essas telas também são composições de 320 px, então ficam
centralizadas com barras. Para o enquadramento original existe `--pillarbox`.

## Verificação

O build compara duas placas construídas dos mesmos fontes com as mesmas opções:
a traduzida e a de referência, que executa o interpretador do FBNeo. Elas
recebem o mesmo roteiro de entradas e precisam coincidir em **todos** os quadros
e blocos de áudio, não só no último:

```text
frames=1200 video=e18569259fdaa2c1/e18569259fdaa2c1
             audio=0eafaed49d1d6063/0eafaed49d1d6063
             capture=e5e436cfe863a8de/e5e436cfe863a8de sites=917504 fallback=0
             => identical
```

`fallback=0` significa que nenhuma instrução precisou ser interpretada: todo o
programa executado passou pelo código traduzido.

No ROG Ally com Bazzite, o atalho da Steam abriu o jogo em uma janela de
1920×1080 com a saída também em 1920×1080, o SDL reconheceu o controle Xbox e o
crédito e o start funcionaram. Uma medição de 30 s de jogo deu lógica a
55,49 fps e apresentação a 60,03 fps, com 134 quadros repetidos e nenhum
descartado. A tradução executou 3.000 quadros em 3,74 s na máquina de compilação
(~800 quadros por segundo), muito acima da cadência necessária.

O inventário do ZIP interno foi produzido pelo REA 6.10.1, evidência
`ev_ab6170b4dcf68263617bfb00ac35e187e1a8d3b6176378b06b714623eea0b808`. A
imagem de programa montada pelo gerador tem SHA-256
`9ff914f142474699925a8d362fd6d8962393c606d70f91d92979a0169dee2d2b`, idêntica
ao `DrvMainROM` que o driver monta em tempo de execução. O vetor de reset lido
em `0xffff0` é `8000:00ea`.

As demais fases, o modo de quatro jogadores e o segundo jogador ainda precisam
de verificação individual.
