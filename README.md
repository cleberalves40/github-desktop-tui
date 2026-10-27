# GitHub Desktop TUI

Cliente Git para terminal, em C++17 e FTXUI, com suporte-alvo a FreeBSD 14.5 e
15.1 e builds via Homebrew.

## Versionamento

A versão do projeto segue SemVer (`MAJOR.MINOR.PATCH`) e é definida em
`project(VERSION ...)` no `CMakeLists.txt`. Enquanto a versão major for `0`, a
compatibilidade da API ainda não é garantida. Releases usam tags Git anotadas
no formato `vMAJOR.MINOR.PATCH`; a versão inicial é `0.1.0` (`v0.1.0`).

## Fase 2: fundação

Esta etapa estabelece o build, a camada de execução de processos e a primeira
visão do estado de um repositório. A interface consulta Git em segundo plano;
nenhum comando é construído ou executado por meio de um shell.

## Dependências

- CMake 3.16 ou superior
- Compilador C++17
- FTXUI
- Git
- GitHub CLI (`gh`, para as próximas etapas)

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

Sem argumento, a aplicação inspeciona o diretório atual. `q` ou `Esc` encerra a
interface.

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
- Branches não são trocadas se o Git detectar mudanças locais conflitantes
- Conflitos de merge são destacados; a resolução continua sendo feita fora da
	TUI nesta etapa

## GitHub CLI

- O estado de autenticação do `gh` aparece no cabeçalho
- `a` verifica autenticação; `A` inicia `gh auth login`
- `p` lista Pull Requests; `P` inicia a criação interativa de um Pull Request
- `:` abre a linha de comando `gh`; argumentos entre aspas são aceitos e o
	histórico é navegável pelas setas
- A saída de comandos não interativos aparece no painel de detalhes
- Comandos interativos recebem o terminal temporariamente e devolvem a TUI ao
	terminar
- Prefixe com `!` (`:!gh auth refresh`) para forçar o handoff em outro comando
- Os comandos são executados com argumentos separados, sem shell; metacaracteres
	não são interpretados como comandos