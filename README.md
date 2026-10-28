# GitHub Desktop TUI

Cliente GitHub para terminal, em C++17 e FTXUI, com suporte-alvo a FreeBSD 14.5
e 15.1 e builds via Homebrew.

## Versionamento

A versão do projeto segue SemVer (`MAJOR.MINOR.PATCH`) e é definida em
`project(VERSION ...)` no `CMakeLists.txt`. Enquanto a versão major for `0`, a
compatibilidade da API ainda não é garantida. Releases usam tags Git anotadas
no formato `vMAJOR.MINOR.PATCH`. A versão inicial foi `0.1.0` (`v0.1.0`);
`0.2.0` adicionou o fluxo de autenticação e clone remoto; `0.3.0` atualizou a
interface; `0.4.0` adicionou sincronização remota com Git; `0.4.1` corrige a
documentação e reforça a cobertura de conflitos durante pull.

## Experiência do aplicativo

O aplicativo pode iniciar sem um repositório local. Na Home, autentique com o
GitHub CLI, navegue pelos repositórios disponíveis e clone uma seleção para
`~/src/OWNER/REPOSITORY`. Também é possível abrir um checkout existente passando
seu caminho como argumento. Comandos são executados com argumentos separados,
sem shell.

## Dependências

- CMake 3.16 ou superior
- Compilador C++17
- FTXUI
- Git
- GitHub CLI (`gh`)

### FreeBSD 14.5 e 15.1

```sh
pkg install cmake ninja ftxui git gh
```

### macOS ou Linux com Homebrew

```sh
brew install cmake ninja ftxui git gh
```

O `gh` é necessário para as integrações com o GitHub; Git, CMake e FTXUI são
necessários para executar e compilar a aplicação.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/github-desktop-tui [caminho-do-repositorio]
```
Sem argumento, a aplicação abre a Home do GitHub e não exige um checkout
local. Execute `gh auth login` ou use `A` na interface para autenticar. `r`
atualiza a lista, `j`/`k` navega e `Enter` clona o repositório selecionado.
Para abrir um checkout existente, passe seu caminho. `q` ou `Esc` encerra a
interface.

## Interface e fontes

A navegação de Changes, History e Branches, a lista de arquivos e o painel de
diff seguem uma organização inspirada no GitHub Desktop. O dock inferior mostra
a saída e a linha de comando do GitHub CLI (`:`); ele não é um shell geral.
Comandos interativos, como `gh auth login`, recebem o terminal temporariamente.

Os ícones usam glyphs Nerd Font. Configure o emulador de terminal para usar
**JetBrainsMono Nerd Font**; o aplicativo não instala nem altera a fonte do
terminal.

## Plataformas e testes

O CI compila e executa CTest em FreeBSD 14.5 e 15.1, além de macOS com pacotes
Homebrew. Os testes usam repositórios temporários, não acessam a rede e removem
seus artefatos ao terminar. A execução local de referência desta etapa ocorreu
em FreeBSD 15.1-STABLE.

## Branches e histórico

- `1`, `2`, `3`: Changes, History e Branches
- Em History, `j`/`k` seleciona commits e mostra detalhes e diff
- Em Branches, `j`/`k` seleciona; `Enter` troca; `c` cria e ativa; `v` compara
	com a branch atual
- `d` solicita confirmação e usa exclusão segura (`git branch -d`); a branch
	atual não pode ser excluída
- `f` busca atualizações (`git fetch`), `l` integra somente fast-forward
	(`git pull --ff-only`) e `u` publica commits (`git push`)
- Pull é bloqueado quando há alterações locais ou conflitos; push nunca usa
	force
- Branches não são trocadas se o Git detectar mudanças locais conflitantes
- Conflitos de merge são destacados; a resolução continua sendo feita fora da
	TUI nesta etapa

## GitHub CLI

- `A` inicia a autenticação e `a` verifica o estado da sessão
- A Home lista até 100 repositórios acessíveis pela conta autenticada
- `r` atualiza a lista; `j`/`k` navega; `Enter` clona em `~/src/OWNER/REPOSITORY`
- `:` abre a linha de comando do GitHub CLI também na Home
- O estado de autenticação do `gh` aparece no cabeçalho
- `p` lista Pull Requests; `P` inicia a criação interativa de um Pull Request
- `:` abre a linha de comando `gh`; argumentos entre aspas são aceitos e o
	histórico é navegável pelas setas
- A saída de comandos Git e GitHub aparece no dock inferior
- Comandos interativos recebem o terminal temporariamente e devolvem a TUI ao
	terminar
- Prefixe com `!` (`:!gh auth refresh`) para forçar o handoff em outro comando
- Os comandos são executados com argumentos separados, sem shell; metacaracteres
	não são interpretados como comandos