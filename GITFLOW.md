# Gitflow deste fork

Este repositório usa `main` como linha estável e `develop` como integração do trabalho ainda não lançado.

## Branches

- `main`: estado publicável; recebe mudanças de `develop` por uma branch `release/*` quando houver uma versão pronta.
- `develop`: base comum das próximas mudanças.
- `feature/<tema>`: nasce de `develop` e volta para `develop` quando a mudança estiver revisada.
- `release/<versão>`: estabilização de uma versão; ao concluir, integra em `main` e `develop` e recebe tag.
- `hotfix/<tema>`: correção urgente criada de `main`; integra em `main` e `develop`.

O repositório original é o remoto `upstream`; o fork do usuário é `origin`. Atualize `develop` do trabalho aceito no fork. Integrações explícitas do repositório original devem ser revisadas antes de avançar a linha de desenvolvimento.

## Trabalho atual

- Fork: `renan-marcel/PS5_PayloadSDK`.
- Feature branch: `feature/dualsense-mic-stream`.
- Objetivo: pesquisar a captura de áudio do DualSense e adicionar uma interface de captura PCM em tempo real, com um exemplo consumível por outros homebrews.

## Ciclo

1. Atualize `develop` a partir de `origin/develop`.
2. Crie `feature/<tema>` a partir de `develop`.
3. Mantenha commits focados; documente evidência e firmware de validação.
4. Revise a branch antes de integrá-la em `develop`.
5. Só promova `develop` para `main` em uma release explicitamente preparada.

Branches de feature são publicadas no fork quando a implementação estiver revisável. A branch `main` não é usada para desenvolvimento direto.
