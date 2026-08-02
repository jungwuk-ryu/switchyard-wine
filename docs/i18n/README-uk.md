<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine logo" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [Інші мови](../README.md#translations)

## Що це таке

Switchyard Wine — це Wine runtime проєкту
[Switchyard](https://github.com/jungwuk-ryu/Switchyard), застосунку для macOS,
який тримає Windows-ігри й лаунчери в керованих контейнерах. Switchyard керує
інтерфейсом, станом контейнера та вибором runtime; цей репозиторій містить Wine-код,
конвеєр збирання та роботу з сумісністю.

Гілка свідомо робить ставку на стабільність: оновлення застосовуються, коли
працюючі сценарії Switchyard перевірено, а не лише тому, що вийшла нова версія
Wine. Будь-які downstream-зміни — звичайні рев’ювані коміти на базі фіксованої
ревізії WineHQ.

## Чому існує ця гілка

- **macOS-логіка в runtime**. У гілці є виправлення для реальних падінь у
  Switchyard: D3DMetal callbacks і resource bridging, синхронізація macOS MSync,
  рендеринг Chromium/CEF, вибір графічного провайдера, медіа відтворення,
  багатомовний fallback шрифтів.
- **Підтверджувана ідентичність runtime**. Збірка фіксує та хешує зовнішні
  вхідні дані, виконується поза активним runtime і публікується лише після
  валідації. Кожен runtime записує ревізію джерела, хеші залежностей, архітектуру
  і основні хеші бінарних файлів.
- **Тести поруч із виправленнями**. Репозиторій регулярно перевіряє D3DMetal,
  native callbacks, MSync, Steam overlay hotpatch, TLS, media, OpenGL і шляхи
  безпеки runtime.
- **Контекстні записи сумісності**. Результати завжди містять точний runtime,
  хост macOS, графічний шлях, дату та відомі обмеження. Запис для однієї
  конфігурації не означає універсальну підтримку.

## MSIX та паковані desktop-додатки

Switchyard Wine поставляє `wineappx` і `appxsvc.dll` для підписаних, не
зашифрованих full-trust desktop MSIX/AppX пакетів. Реалізований цикл містить
перевірку, витягування з верифікацією, інсталяцію/оновлення в префіксі,
видалення, відновлення, збір сміття та запуск проголошених Win32/WinUI 3 додатків
з ідентичністю пакета й статичними залежностями.

Це не повний стек Windows: це не клієнт Microsoft Store, не UWP/AppContainer,
не обхід перевірки підпису для непідписаних пакетів і не універсальна гарантія
сумісності всіх Windows App SDK API. Точні вимоги до пакетів, команд,
моделі надійності й поточних обмежень описані в
[MSIX-гайді](../msix.md).

## Дивитися в роботі

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="Switchyard container library showing Windows games and launchers on macOS" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced and Rockstar Games Launcher running in Switchyard" width="100%">
  <br>
  <sub>GTA V Enhanced і Rockstar Games Launcher в одному керованому Wine-сеансі.</sub>
</p>

## Отримати або зібрати runtime

Користувачі Switchyard зазвичай керують контейнерами та вибором runtime
безпосередньо через застосунок. Підписані й нотаризовані Wine-only архіви доступні в
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases).
Apple Game Porting Toolkit надається користувачем і не розповсюджується в цьому
репозиторії або релізах.

Для збирання з вихідного коду на Apple Silicon macOS:

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

Перед публікацією або заміною runtime обов’язково перегляньте
[гід збірки](../building.md): інструментарій, перевірені залежності, staging,
підпис, нотаризацію та доступні регресійні тести.

## Документація

- [Індекс документації](../README.md)
- [Архітектура та межі репозиторію](../architecture.md)
- [Збірка та реліз runtime](../building.md)
- [MSIX і підтримка пакованих desktop-додатків](../msix.md)
- [Результати сумісності застосунків](../compatibility.md)
- [Provenance джерела та залежностей](../provenance.md)
- [Усунення проблем Unity-ігор](../troubleshooting-unity-games.md)

Для загального користування Wine дивіться
[документацію WineHQ](https://gitlab.winehq.org/wine/wine/-/wikis/home)
та [джерело upstream](https://gitlab.winehq.org/wine/wine). Цей README описує
лише Switchyard Wine, а не посібник upstream Wine.

## Спільнота та ліцензія

Приєднуйтесь до [Switchyard Discord](https://discord.gg/USNfzUza7B)
для тестування runtime, звітів про сумісність і обговорення розробки.

Wine і зміни Switchyard Wine ліцензовані за LGPL-2.1-or-later; дивіться
`LICENSE` і `COPYING.LIB`.

Switchyard Wine є незалежним проєктом і не має офіційної підтримки з боку
WineHQ, Apple, Microsoft або продуктів, що показані вище.
