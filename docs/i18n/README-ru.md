<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine logo" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [Другие языки](../README.md#translations)

## Что это такое

Switchyard Wine — это Wine runtime для проекта
[Switchyard](https://github.com/jungwuk-ryu/Switchyard), macOS-приложения для
запуска Windows-игр и лаунчеров в управляемых контейнерах. Switchyard отвечает
за интерфейс, состояние контейнеров и выбор runtime; этот репозиторий отвечает за код
Wine, конвейер сборки и работу по совместимости.

Проект сознательно выбирает стабильность: обновления применяются после проверки
установившихся сценариев Switchyard, а не только потому, что вышла новая версия
Wine. Изменения в downstream выполняются обычными ревьюруемыми коммитами на основе
фиксированной ревизии WineHQ.

## Почему этот форк

- **Работа macOS в runtime**. Ветка содержит исправления для реальных путей сбоев
  Switchyard: D3DMetal callback/resource bridge, macOS MSync, рендеринг Chromium/CEF,
  выбор графического провайдера, воспроизведение медиа, мультиязычный fallback шрифтов.
- **Runtime с проверяемой идентичностью**. Сборки фиксируют и хешируют внешние
  артефакты, компонуются вне активного runtime и публикуются только после
  верификации. Для каждого runtime фиксируются ревизия исходников, хеши зависимостей,
  архитектуры и основные бинарные хеши.
- **Тесты рядом с исправлениями**. Поддерживаются проверки D3DMetal, нативных
  callbacks, MSync, Steam overlay hotpatch, TLS, media, OpenGL и путей безопасности
  runtime.
- **Совместимость с контекстом**. Каждый результат указывает конкретный runtime,
  macOS-окружение, графический путь, дату и известные ограничения. Результат для
  одной конфигурации не означает универсальной поддержки.

## MSIX и упакованные desktop-приложения

Switchyard Wine включает `wineappx` и `appxsvc.dll` для подписанных, незашифрованных
full-trust desktop MSIX/AppX пакетов. Реализован жизненный цикл: проверка,
извлечение с верификацией, установка/обновление по префиксу, удаление, восстановление,
garbage collection и запуск объявленных Win32/WinUI 3 приложений с идентичностью пакета
и статическими зависимостями.

Этот объём поддержки намеренно уже, чем в Windows. Это не клиент Microsoft Store,
не UWP/AppContainer, не обход для неподписанных пакетов и не «поддержка всех Windows
App SDK API».
Точные требования к пакету, командам, модели устойчивости и текущим лимитам описаны
в [MSIX-гайде](../msix.md).

## Как это выглядит

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="Switchyard container library showing Windows games and launchers on macOS" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced and Rockstar Games Launcher running in Switchyard" width="100%">
  <br>
  <sub>GTA V Enhanced и Rockstar Games Launcher в одном управляемом Wine-сеансе.</sub>
</p>

## Получить или собрать runtime

Обычно пользователи Switchyard управляют runtime и контейнерами через само
приложение. Подписанные и нотариально заверенные Wine-only архивы доступны в
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases).
Apple Game Porting Toolkit предоставляется пользователем и не включается в этот
репозиторий или релизы.

Сборка из исходников на Apple Silicon macOS:

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

Перед публикацией или заменой runtime посмотрите
[гайд сборки](../building.md), где описаны требуемые инструменты, проверяемые
зависимости, подготовка, подпись, нотаризация и доступные регрессионные проверки.

## Документация

- [Индекс документации](../README.md)
- [Архитектура и границы репозитория](../architecture.md)
- [Сборка и релиз runtime](../building.md)
- [MSIX и упакованные desktop-приложения](../msix.md)
- [Результаты совместимости приложений](../compatibility.md)
- [Provenance исходников и зависимостей](../provenance.md)
- [Рекомендации по Unity-приложениям](../troubleshooting-unity-games.md)

Для общего использования Wine обращайтесь к
[документацию WineHQ](https://gitlab.winehq.org/wine/wine/-/wikis/home) и
[исходники Wine](https://gitlab.winehq.org/wine/wine). Этот README описывает
только Switchyard Wine, а не руководство по upstream Wine.

## Сообщество и лицензия

Для обсуждения тестирования runtime, отчетов о совместимости и разработки
присоединяйтесь к [Switchyard Discord](https://discord.gg/USNfzUza7B).

Wine и изменения Switchyard Wine лицензированы по LGPL-2.1-or-later; см.
`LICENSE` и `COPYING.LIB`.

Switchyard Wine — самостоятельный проект и не является официально одобренным
WineHQ, Apple, Microsoft или продуктами, показанными выше.
