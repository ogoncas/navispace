# NaviSpace

Indicador de espaços de trabalho para o painel XFCE, desenvolvido em GTK3, com animações suaves baseadas em física e diversos estilos visuais altamente customizáveis.

## Recursos

- **5 estilos visuais:** números (`1 2 3`), nomes de workspaces, algarismos romanos (`I II III`), pontos (pílula) e traços.
- **3 formas de destaque** para o espaço ativo: preenchido, contorno ou sublinhado.
- **Marcadores de janelas** nos espaços ocupados: barra inferior, ponto central ou nenhum.
- **4 tipos de movimento** para a animação do destaque: deslizar, esticar, mola (spring) e desvanecer (fade).
- **Alerta inteligente:** espaços com janelas que solicitam atenção ficam destacados em vermelho, com opção de efeito de pulso suave.
- **Cores flexíveis:** utilize uma cor de destaque própria (configurável por seletor RGB) ou herde automaticamente a cor de seleção do tema GTK ativo.
- **Opções avançadas:** ocultar espaços vazios, ajuste de arredondamento (`radius`), espaçamento (`gap`) e duração da animação em milissegundos.
- **Interatividade completa:** clique para alternar de espaço, use a roda do mouse para navegar rapidamente (com suporte opcional a loop/volta ao fim) e visualize tooltips informativos com a contagem exata de janelas.
- **Compatibilidade total:** suporta painéis horizontais e verticais, respeitando a configuração global de animações do sistema GTK.

## Requisitos

- Sessão X11 (utiliza a biblioteca `libwnck`, que não opera nativamente em ambientes Wayland puros)
- XFCE com `xfce4-panel` versão 4.14 ou superior
- Ferramentas de compilação e desenvolvimento: `gcc`, `pkg-config`, juntamente com os pacotes de desenvolvimento do GTK 3, `libxfce4panel`, `libwnck` e `libxfce4util`.

### Instalação das dependências (Debian / Ubuntu / Mint)

```
sudo apt install build-essential libgtk-3-dev libxfce4panel-2.0-dev libwnck-3-dev libxfce4util-dev
