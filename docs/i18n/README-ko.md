<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine 로고" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [다른 언어](../README.md#translations)

Switchyard Wine은 macOS 앱
[Switchyard](https://github.com/jungwuk-ryu/Switchyard)에서 쓰는 Wine
런타임입니다. Switchyard 앱은 화면과 컨테이너 상태, 런타임 선택을 맡고, 이
저장소는 그 아래에서 돌아가는 Wine 코드와 빌드 과정, 호환성 수정 사항을
관리합니다.

새 Wine 버전이 나왔다고 바로 따라가지는 않습니다. Switchyard에서 이미 잘
돌아가는 게임과 런처를 다시 확인한 뒤 기준 버전을 올립니다. 프로젝트 고유의
수정 사항은 WineHQ의 고정된 리비전 위에 일반 Git 커밋으로 남겨, 무엇을 왜
바꿨는지 살펴볼 수 있게 했습니다.

## 이 런타임을 따로 만드는 이유

- **macOS에서 실제로 부딪힌 문제를 고칩니다.** D3DMetal 콜백과 리소스 연결,
  macOS MSync 동기화, Chromium/CEF 화면 표시, 그래픽 백엔드 선택, 미디어 재생,
  다국어 글꼴 대체처럼 Switchyard에서 확인된 경로를 다룹니다.
- **어떤 소스로 만든 런타임인지 추적할 수 있습니다.** 외부 입력은 버전과 해시를
  고정하고, 사용 중인 런타임과 떨어진 곳에서 빌드한 다음 검증이 끝나야
  교체합니다. 결과물에는 소스 리비전과 의존성, 아키텍처, 핵심 바이너리 해시가
  기록됩니다.
- **컴파일 성공만으로 끝내지 않습니다.** D3DMetal, 네이티브 콜백, MSync,
  Steam 오버레이 핫패치, TLS, 미디어, OpenGL, 런타임 안전 경로를 각각
  회귀 테스트합니다.
- **호환성 결과에 조건을 함께 적습니다.** 확인한 런타임과 macOS 환경, 그래픽
  경로, 날짜, 알려진 제한을 남깁니다. 한 환경에서 실행됐다는 결과를 전체 지원
  약속처럼 표현하지 않습니다.

## MSIX와 패키지형 데스크톱 앱

`wineappx`와 `appxsvc.dll`은 서명됐고 암호화되지 않은 full-trust 데스크톱
MSIX/AppX 패키지를 다룹니다. 패키지 검사와 검증된 압축 해제, 프리픽스별 설치와
업데이트, 삭제, 복구, 가비지 컬렉션을 수행하고, 패키지 ID와 정적 의존성을 붙여
Win32 또는 WinUI 3 앱을 실행할 수 있습니다.

범위는 Windows보다 좁습니다. Microsoft Store 클라이언트나 UWP/AppContainer,
서명 우회 기능을 구현한 것이 아니며, 모든 Windows App SDK API가 동작한다는
뜻도 아닙니다. 지원하는 패키지 조건과 명령어, 저장소의 내구성 방식, 아직
지원하지 않는 기능은 [MSIX 안내서](../msix.md)에 정리돼 있습니다.

## 실행 화면

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="macOS에서 Windows 게임과 런처를 관리하는 Switchyard 컨테이너 목록" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Switchyard에서 실행 중인 Grand Theft Auto V Enhanced와 Rockstar Games Launcher" width="100%">
  <br>
  <sub>하나의 Wine 세션에서 실행 중인 Grand Theft Auto V Enhanced와 Rockstar Games Launcher.</sub>
</p>

## 런타임 받기와 직접 빌드하기

일반 사용자는 Switchyard 앱에서 컨테이너와 런타임을 관리하면 됩니다. 서명과
공증을 거친 Wine 전용 압축 파일은
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases)에서
받을 수 있습니다. Apple Game Porting Toolkit은 사용자가 직접 준비해야 하며,
이 저장소와 릴리스에는 들어 있지 않습니다.

Apple Silicon Mac에서 소스로 빌드하려면 다음 명령을 실행합니다.

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

런타임을 배포하거나 교체하기 전에는 [빌드 안내서](../building.md)를 읽어 주세요.
필요한 도구와 검증된 의존성, 스테이징, 서명과 공증, 회귀 테스트가 설명돼
있습니다.

## 문서

- [문서 목록](../README.md)
- [구조와 저장소 경계](../architecture.md)
- [런타임 빌드와 릴리스](../building.md)
- [MSIX와 패키지형 데스크톱 지원](../msix.md)
- [앱 호환성 기록](../compatibility.md)
- [소스와 의존성 출처](../provenance.md)
- [Unity 게임 문제 해결](../troubleshooting-unity-games.md)

일반적인 Wine 사용법과 개발 자료는 [WineHQ 문서](https://gitlab.winehq.org/wine/wine/-/wikis/home)와
[upstream 소스](https://gitlab.winehq.org/wine/wine)를 참고하세요. 이 문서는
Wine 설명서를 되풀이하지 않고 Switchyard Wine에서 달라진 부분만 소개합니다.

## 커뮤니티와 라이선스

런타임 테스트와 호환성 제보, 개발 이야기는
[Switchyard Discord](https://discord.gg/USNfzUza7B)에서 나눌 수 있습니다.

Wine과 Switchyard Wine의 변경 사항은 LGPL-2.1-or-later로 배포됩니다. 자세한
내용은 `LICENSE`와 `COPYING.LIB`을 확인하세요. Switchyard Wine은 독립
프로젝트이며 WineHQ, Apple, Microsoft 또는 위 화면에 나온 제품의 보증을 받지
않습니다.
