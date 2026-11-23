# Zero Team USA no ROG Ally

Port Linux x86-64 do arcade da Seibu Kaihatsu (1993), feito a partir da edição
**Zero Team USA (US)**, conjunto `zeroteam`. O programa V30 do jogo é traduzido
estaticamente para x86-64 durante a compilação, e a placa usa os dispositivos
Seibu do FBNeo para memória, vídeo, som e entrada. O executável e a biblioteca
são específicos deste título; a compilação usa a ROM do próprio jogador.

## Jogar no Ally

O atalho **Zero Team USA** está na biblioteca da Steam. A instalação fica em
`~/Games/ZeroTeam-Native` (NVMe interno; o cartão microSD estava somente
leitura no dia da instalação). O jogo apresenta a arena ampliada em tela cheia
a 1920×1080. Os sprites conservam a proporção do gabinete.

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

A área de jogo tem 428×256 pixels de arcade e a saída tem 1920×1080. O mapa circular da placa guarda 32 colunas de tiles. Os dois formatos de mapa do jogo agora carregam até 28 colunas de 16 pixels, incluindo os tiles parcialmente visíveis nas bordas. A câmera acompanha a origem do trecho jogável no porto e na montanha. A sala fechada do dojo usa os dois vãos da parede para ampliar o salão com os tiles da própria fase.

Os limites laterais de movimento, as verificações de visibilidade, o descarte de objetos e a montagem dos sprites consideram a área ampliada. O título, a seleção de personagem e a tabela de recordes usam as texturas das bordas de seus próprios fundos. O texto, os retratos e os lutadores continuam em suas proporções originais. A opção `--pillarbox` recupera a janela e os limites de movimento do gabinete.

## Verificação no Ally

O teste da primeira luta segura o direcional e lê a posição do jogador enquanto ele encosta nas duas extremidades. Em coordenadas relativas à câmera do gabinete, o modo clássico atingiu 16 e 304. Em 16:9 o mesmo roteiro atingiu −38 e 358. Isso acrescenta 54 pixels jogáveis por lado. Para repetir depois da compilação, execute o comando abaixo na pasta do projeto.

```sh
python3 tools/verify_edges.py .work/build/libzeroteam_board.so .work/private/chips
```

A versão traduzida e a placa de referência coincidiram em vídeo e áudio durante 5.000 quadros com as mesmas entradas. A tradução registrou 917.504 endereços e nenhum recurso ao interpretador. As verificações visuais incluíram a primeira fase, o porto, a montanha, telas fixas e a composição do dojo com os mapas da ROM. Na execução isolada da versão final no Ally, a saída SDL mediu 1920×1080, com 55,43 atualizações de jogo e 59,86 apresentações por segundo em 1.250 quadros. O atalho da Steam também foi conferido no painel durante a revisão.

O inventário do ZIP interno foi produzido pelo REA 6.10.1, evidência
`ev_ab6170b4dcf68263617bfb00ac35e187e1a8d3b6176378b06b714623eea0b808`. A
imagem de programa montada pelo gerador tem SHA-256
`9ff914f142474699925a8d362fd6d8962393c606d70f91d92979a0169dee2d2b`, idêntica
ao `DrvMainROM` que o driver monta em tempo de execução. O vetor de reset lido
em `0xffff0` é `8000:00ea`.

## Campanha completa em 16:9

A revisão da campanha cobre as seis fases, seus chefes, os dois bônus, o resgate de helicóptero, a cerimônia e os créditos. No cais e na pedreira, o renderizador guarda as colunas originais que já passaram pela câmera. Assim a borda oposta do mapa não reaparece quando a rolagem termina. Nos salões do castelo, a câmera usa a extensão real da sala. Onde a pintura foi desenhada apenas para a largura do gabinete, as faixas externas reutilizam a borda do cenário. Os inimigos e efeitos são compostos depois, em suas coordenadas de jogo.

O teste de regressão reproduz os comandos do jogador 1 desde a tela inicial. Ele usa créditos livres e repõe a energia no processo de teste para atravessar a campanha inteira. O atalho da Steam usa as regras normais. O arquivo de entradas contém a duração de cada comando em quadros e a máscara hexadecimal dos botões. A ferramenta confere as transições da campanha até os créditos e calcula assinaturas do áudio e de amostras de vídeo. Para repetir com os chips da edição USA, execute a partir da pasta do projeto.

```sh
python3 tools/verify_campaign.py .work/build/libzeroteam_board.so .work/private/chips
```

No Ally, as placas traduzida e de referência completaram os 26 marcos no mesmo quadro. O roteiro terminou após 187.028 quadros, já nos créditos. As assinaturas coincidiram em vídeo (`e5ac8577`, uma amostra a cada 60 quadros e em cada marco) e áudio (`bd3b153f`, todas as amostras). A tradução usou zero fallbacks. Em uma execução SDL separada de 600 quadros em tela virtual de 1920×1080, a lógica ficou em 55,47 Hz e a apresentação em 60,00 Hz, sem descarte.

A verificação cobre um jogador. O modo cooperativo continua sem teste de campanha completa.
