<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine logo" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [Diğer diller](../README.md#translations)

Switchyard Wine, [Switchyard](https://github.com/jungwuk-ryu/Switchyard) uygulamasının ardındaki Wine runtime'dır. Switchyard uygulaması arayüzü, konteyner durumu ve runtime seçimini yönetir; bu depo ise Wine kodunu, derleme hattını ve Switchyard odaklı uyumluluk çalışmalarını yönetir.

Proje bilinçli bir seçim yapar: her yeni Wine sürümünde otomatik olarak güncelleme yapmaz; Switchyard'ın onaylanmış yük akışları doğrulanana kadar mevcut Wine tabanında kalır. Downstream değişiklikler, pinlenmiş bir WineHQ revizyonu üstünde normal, denetlenebilir Git commitleri olarak tutulur.

## Bu dalın nedeni

- **runtime içi macOS geliştirmeleri.** Bu dal, Switchyard'de gerçek hata akışlarına yönelik
  düzeltmeler içerir: D3DMetal callback ve kaynak köprüleme, macOS MSync eşitlemesi,
  Chromium/CEF renderı, grafik sağlayıcı seçimi, medya oynatma ve çok dilli yazı tipi geri
  dönüşü.
- **Doğrulanabilir runtime kimliği.** Build girdileri pinlenir ve hash'lenir; build aktif
  runtime dışındaki bir ortamda yapılır ve doğrulama sonrası yayınlanır. Her runtime, kaynak
  revizyonu, dependency digest'leri, mimari ve çekirdek ikililerin hash'lerini kaydeder.
- **Regresyon testleri yanında.** Depo D3DMetal, native callback, MSync, Steam overlay
  hotpatch, TLS, medya, OpenGL ve runtime güvenlik yollarını test eder; yalnızca başarılı bir derlemeye güvenmez.
- **Bağlamlı uyumluluk kayıtları.** Sonuçlar tam runtime, macOS host, grafik yolu, tarih ve
  bilinen kısıtlamaları içerir; tek bir ortamda çalışmak evrensel destek anlamına gelmez.

## MSIX ve paketlenmiş masaüstü uygulamaları

Switchyard Wine, imzalı, şifrelenmemiş, full-trust desktop MSIX/AppX paketlerini işlemek için
`wineappx` ve `appxsvc.dll` içerir. Uygulama yaşam döngüsü; doğrulama, doğrulanmış çıkarma,
önek bazlı kurulum ve güncelleme, kaldırma, geri kazanma, çöp toplama ve paket kimliği
ile statik bağımlılıklara sahip ilan edilmiş Win32 veya WinUI 3 uygulamalarının başlatılmasını kapsar.

Bu kapsam kasıtlı olarak Windows'tan daha dardır. Bu bir Microsoft Store istemcisi değildir,
UWP/AppContainer desteği değildir, imzasız paket denetimini atlatmaz ve
tüm Windows App SDK API'lerinin çalıştığına dair bir vaad de içermez. Tam paket gereksinimleri,
komutlar, dayanıklılık modeli ve güncel sınırlamalar [MSIX kılavuzunda](../msix.md) yer alır.

## Çalışırken görünümü

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="Switchyard container library macOS üzerinde Windows oyunları ve launcherlara bakış" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced ve Rockstar Games Launcher'ın Switchyard'de çalışması" width="100%">
  <br>
  <sub>Tek bir yönetilen Wine oturumunda Grand Theft Auto V Enhanced ve Rockstar Games Launcher.</sub>
</p>

## Runtime edinme veya derleme

Switchyard kullanıcıları genellikle konteynerleri ve runtime seçimini uygulamanın yönetmesine izin verir.
İmzalanmış ve notarize edilmiş Wine-only arşivler
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases)'te mevcuttur.
Apple Game Porting Toolkit, kullanıcı tarafından sağlanan bir yazılımdır ve ne bu depoya ne de
yayınlara dahil edilmez.

Apple Silicon macOS'ta kaynaktan derleme:

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

Runtime yayınlamadan veya değiştirmeden önce [build rehberi](../building.md)'ni okuyun; gerekli
toolchain, doğrulanmış bağımlılıklar, staging, imzalama, notarizasyon ve kullanılabilir regresyon
kontrollerini kapsar.

## Dokümantasyon

- [Dokümantasyon dizini](../README.md)
- [Mimari ve depo sınırları](../architecture.md)
- [Runtime derleme ve yayınlama](../building.md)
- [MSIX ve paketlenmiş masaüstü desteği](../msix.md)
- [Kayıtlı uygulama uyumluluğu](../compatibility.md)
- [Kaynak ve bağımlılık provenansı](../provenance.md)
- [Unity oyun hata ayıklama](../troubleshooting-unity-games.md)

Genel Wine kullanımı ve geliştirme için
[WineHQ dokümantasyonu](https://gitlab.winehq.org/wine/wine/-/wikis/home) ve
[upstream kaynak](https://gitlab.winehq.org/wine/wine) sayfalarını kullanın. Bu README yalnızca
Switchyard Wine'i açıklar, upstream Wine kılavuzunu kopyalamaz.

## Topluluk ve lisans

Runtime testleri, uyumluluk raporları ve geliştirme sohbeti için
[Switchyard Discord](https://discord.gg/USNfzUza7B) kanalına katılın.

Wine ve Switchyard Wine değişiklikleri LGPL-2.1-or-later ile lisanslanmıştır; ayrıntılar için
`LICENSE` ve `COPYING.LIB` dosyalarına bakın. Switchyard Wine bağımsızdır ve WineHQ,
Apple, Microsoft veya yukarıdaki ürünler tarafından onaylanmamıştır.
