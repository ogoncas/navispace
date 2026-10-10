# NaviSpace

Indicador de espaços de trabalho para o painel XFCE, com animações suaves e vários estilos visuais.

## Recursos

- **5 estilos:** números, nomes, algarismos romanos, pontos (pílula) e traços.
- **3 formas de destaque** do espaço ativo: preenchido, contorno ou sublinhado.
- **Marcador de janelas** nos espaços ocupados: barra, ponto ou nenhum.
- **4 movimentos** do destaque: deslizar, esticar, mola e desvanecer.
- **Alerta:** espaços com janelas pedindo atenção ficam em vermelho, com pulso opcional.
- **Cor de destaque** própria ou a do tema GTK.
- **Ocultar espaços vazios**, arredondamento, espaçamento e duração da animação ajustáveis.
- Clique para trocar de espaço, roda do mouse para navegar (com ou sem volta ao fim) e tooltip com a contagem de janelas.
- Funciona em painéis horizontais e verticais. Respeita a opção global de animações do GTK.

## Requisitos

- Sessão X11 (usa o libwnck, que não funciona em Wayland puro)
- XFCE com `xfce4-panel` 4.14 ou superior
- Para compilar: `gcc`, `pkg-config` e os pacotes de desenvolvimento do GTK 3, libxfce4panel, libwnck e libxfce4util

No Debian/Ubuntu:

```bash
sudo apt install build-essential libgtk-3-dev libxfce4panel-2.0-dev libwnck-3-dev libxfce4util-dev
```

## Instalação

```bash
git clone https://github.com/ogoncas/navispace
cd navispace
sudo bash compilador.sh
```

O script compila em um diretório temporário e instala a biblioteca e o arquivo `.desktop` nos diretórios do painel. Ele **não reinicia o painel**: se for necessário, avisa, e você executa como usuário comum:

```bash
xfce4-panel -r
```

Na primeira instalação, adicione o plugin em **Painel → Itens → Adicionar → NaviSpace**.

Para remover:

```bash
sudo bash compilador.sh --uninstall
```

## Configuração

Clique com o botão direito no plugin e escolha **Propriedades**. As alterações são aplicadas e salvas na hora.

| Seção | Opções |
|---|---|
| Espaços de trabalho | quantidade, ocultar vazios, roda do mouse dá a volta |
| Aparência | estilo, destaque, marcador de janelas, cor (própria ou do tema), arredondamento, espaçamento |
| Animação | movimento, duração (0 desativa), pulsar em alerta |

Em painéis verticais, o estilo "Nomes" usa números, porque os nomes são largos demais.

## Estrutura

| Arquivo | Função |
|---|---|
| `navispace.c` | código-fonte do plugin |
| `navispace.desktop` | descrição do plugin para o painel |
| `compilador.sh` | compila, instala e remove |

## Solução de problemas

- **O plugin não aparece na lista:** reinicie o painel com `xfce4-panel -r`.
- **Aparece "NaviSpace indisponível":** a sessão não é X11 ou o gerenciador de janelas não suporta espaços de trabalho.
- **Erro de dependência ao compilar:** instale os pacotes de desenvolvimento listados em Requisitos.
