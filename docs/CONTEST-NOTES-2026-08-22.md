# Poznámky z reálného nasazení — závod 2026-08-22 (YO DX HF)

Testovaná binárka: **skimmer-for-linux 0.3.0**, `builddir/skimmer-for-linux`,
git `107172f`. Běželo ~2,5 h (21:33 ukončeno), TCI klient proti
sdr-for-linux 0.4.1 na `ws://127.0.0.1:40001`.

Sesterský dokument s poznámkami k deníku:
`log-for-linux/docs/CONTEST-NOTES-2026-08-22.md`.

Položky **N1/N2 nejsou Richardova hlášení** — jsou to nálezy ze stderr
logu běhu, které za provozu nebyly vidět (aplikace psala do logu, ne na
obrazovku). Zapsáno k triage stejně jako hlášené věci.

---

## Poznámky

### N1. Časovače tikají po zničení okna — dvě GTK-CRITICAL při zavření
**Nález (log, ne hlášení):** při ukončení aplikace v 21:33:56.045 padly
ve stejné milisekundě dvě kritické hlášky:

```
Gtk-CRITICAL: gtk_label_set_text: assertion 'GTK_IS_LABEL (self)' failed
Adwaita-CRITICAL: adw_window_title_set_subtitle: assertion
                  'ADW_IS_WINDOW_TITLE (self)' failed
```

Poslední provozní zpráva před nimi je z 21:16:33 (TX hold), pak 17 minut
ticha — takže to není následek něčeho za provozu, ale čistě teardown.

**Vyhodnocení:** závažnost **hygiena / latentní riziko**. Reálně dnes
neškodí (aplikace se zavírá, GTK to zachytí assertem), ale sahá se na
zničený objekt — a to je stav, který se při jiném pořadí destrukce může
projevit jako pád, ne jako hláška.

Příčina: `main.c:1718–1740` registruje čtyři periodické zdroje —
`status_tick` (1 s), `age_tick` (2 s), `scan_tick` (3 s) a debug
`lag_tick` (250 ms) — a **žádný z nich se při zavření okna neodregistruje**.
Callbacky sahají na widgety uložené v `app` (`gtk_label_set_text(app->status, …)`
na `main.c:801` a `:805`, `adw_window_title_set_subtitle(app->title, …)`
na `main.c:601`, `:611`, `:1081`, `:1094`). Když okno zmizí dřív, než
doběhne poslední tik, ukazatele míří na zničené objekty. Grep na
teardown guardy v `src/app/` nevrací nic.

**Vzor je přitom v rodině hotový.** `log-for-linux` řeší přesně tohle
dvěma způsoby najednou: idle payload drží **silnou referenci** na okno
(`win.c:484` — komentář *„strong ref — idle may outlive the window"*)
a callback na vstupu kontroluje rozpadlé okno
(`win.c:433`: *„Drop if the window is already tearing down"*, test
`if (self->tci_label == NULL) … return G_SOURCE_REMOVE`).

**Návrh opravy (příští verze):** převzít ten samý vzor — držet si ID
vrácená `g_timeout_add*` a odstranit je v teardownu okna
(`g_clear_handle_id (&app->status_tick_id, g_source_remove)`), plus
pojistka na vstupu každého ticku (`if (!GTK_IS_LABEL (app->status))
return G_SOURCE_REMOVE;`). Ověření: zavřít appku za běhu s dekódováním
a mít log bez jediné CRITICAL.

### N2. GtkImage baseline warnings — v obou appkách
**Nález (log, ne hlášení):** 18 hlášek ve skimmeru a 65 v sdr-for-linux,
všechny stejného tvaru:

```
Gtk-WARNING: GtkImage 0x… reported baselines of minimum -1 and natural -1,
but sizes of minimum N and natural N. Baselines must be inside the widget size.
```

Pokaždé jiná adresa objektu → jde o mnoho různých instancí `GtkImage`,
ne o jeden zlobivý widget.

**Vyhodnocení:** závažnost **kosmetická** (nic vizuálně rozbitého Richard
nehlásil), ale objem je nápadný a zanáší log — dnes to byla většina všeho,
co obě appky do stderr napsaly.

⚠️ **Neověřeno:** nedohledal jsem, které konkrétní `GtkImage` to jsou ani
zda jde o chybu v našem kódu, nebo o regresi v GTK 4.22 (běželo proti
GTK 4.22, GL renderer). To, že se to objevuje **v obou appkách naráz**
a v obou byla mezitím vyměněná ikonografie, ukazuje spíš na sdílený vzor
použití ikon než na náhodu — ale je to domněnka, ne závěr.

**Návrh (příští verze):** doběhnout to nejdřív diagnosticky, ne opravou —
spustit s `G_DEBUG=fatal-warnings` pod `gdb` a nechat si vypsat backtrace
prvního výskytu; to jednoznačně určí místo i to, jestli je naše. Teprve
podle výsledku rozhodnout, kam oprava patří (nebo že patří do GTK).

**Pointer:** totéž se týká `sdr-for-linux` — viz jeho
`docs/CONTEST-NOTES-2026-08-22.md`, položka N1.

---

# 2. den — 23. 8. 2026

Binárka **beze změny** (`builddir/skimmer-for-linux`, git `107172f`).
Spuštěno 12:23 z build stromu, TCI klient proti sdr-for-linux na
`ws://127.0.0.1:40001` (log potvrzuje `tci: client 0/1 connected`).

**Log 2. dne** (`/var/tmp/contest-2026-08-23-logy/skimmer.log`): 108 řádků,
**všech 108 jsou provozní `pipeline: TX hold — decode frozen (own
transmission)` / `TX hold released`** — tj. korektní zamrznutí dekódu
při vlastním vysílání, 54 párů, žádný nespárovaný hold. Jinak **nula**
warningů, criticalů a assertů.

### N2 — aktualizace: 2. den ani JEDEN GtkImage baseline warning
1. den 18 hlášek, 2. den **0**. Přitom:

- **stejná binárka** (git `107172f`, nepřebuildováno),
- **stejné GTK** — `gtk4 1:4.22.4-1`, nainstalováno 7. 6., od té doby
  neaktualizováno; `libadwaita 1:1.9.3-1` z 5. 8.; **žádná pacman
  transakce 22.–23. 8.**,
- **stejné prostředí** — `XDG_DATA_DIRS`, `GTK_THEME`, `GSK_RENDERER`,
  `GDK_SCALE`, `GDK_DPI_SCALE`, `XCURSOR_SIZE`, `GTK_A11Y`, `LANG`
  porovnány mezi procesem appky a GNOME session: **bez rozdílu**.

Totéž v `sdr-for-linux` (1. den 65×, 2. den 0×) — viz jeho poznámky.

**Co z toho plyne pro triage:** warning **není deterministický** napříč
spuštěními téže binárky. Slepá oprava „podle adresy widgetu" je tím pádem
vyloučená a doporučení z N2 (nejdřív backtrace přes
`G_DEBUG=fatal-warnings` pod `gdb`, teprve pak rozhodovat) platí tím spíš.
Zároveň klesá priorita: dvě z pěti provozních hodin appka nezalogovala
nic.

⚠️ **Domněnka, neověřeno:** vypadá to na závislost na časování prvního
vykreslení (studená vs. teplá icon-cache) — 2. den se spouštělo do už
rozběhnuté session s načtenými ikonami. Neprokázáno; k prokázání by bylo
třeba opakovaně spustit se shozenou GTK icon cache a sledovat, jestli se
hlášky vrátí.

### N1 — stále nereprodukováno v tomto běhu
Appka v době psaní **ještě běží** (spuštěna 12:23). Teardown CRITICALy
z 1. dne se tedy zatím objevit nemohly. Její stderr míří do
`/var/tmp/contest-2026-08-23-logy/skimmer.log` — **až ji Richard zavře,
je v tom souboru zdarma reprodukční test N1**: buď tam ty dvě hlášky
(`gtk_label_set_text` / `adw_window_title_set_subtitle`) budou, nebo ne.
Stojí za to log po zavření zkontrolovat, než se sáhne na kód.

---

# Rozbor 5. 9. 2026

### N1 — reprodukováno, opraveno
- **Log 2. dne** (`/var/tmp/contest-2026-08-23-logy/skimmer.log`, instance
  už zavřená): 108 provozních řádků TX hold, **0 CRITICAL**. Nedeterminismus
  se potvrdil — je to souhra okolností, ne konstanta.
- **Mechanismus** (zdroje GTK 4.22 `gtkwindow.c`, GLib 2.88 `gapplication.c`):
  `gtk_window_close` → `gtk_window_destroy` proběhne synchronně uvnitř
  dispatch close-requestu; `g_application_run` dokončí dispatch list
  **téhož** průchodu smyčky a teprve pak zjistí, že aplikace pustila poslední
  okno. Každý tick, který byl due spolu s událostí zavření, tedy běžel nad
  finalizovanými widgety. Odtud „dvě CRITICAL v jedné milisekundě" — a odtud
  i nula hlášek 2. dne: den 1 se zavření trefilo do sekundové hranice
  časovačů, den 2 ne.
- **Deterministická reprodukce** (headless Broadway `:7`, izolovaná
  `XDG_CONFIG_HOME` s hostem `skimmer-test.invalid` — na živé sdr-for-linux,
  které naslouchá na `0.0.0.0:40001`, se sáhnout nesmělo; skripty v
  `/var/tmp/skimmer-skm12/`): gdb zastaví proces v `status_tick` na 4 s
  (všechny sekundové časovače se stanou due), v následujícím `age_tick`
  zavolá `gtk_window_close`; `scan_tick` ve stejném dispatch listu vypíše
  **přesně tu `adw_window_title_set_subtitle` CRITICAL z 1. dne**. Po
  zavření `g_type_check_instance_is_a` nad `app->status` i `app->title`
  vrací 0 — objekty jsou finalizované, takže původně navržená pojistka
  `GTK_IS_LABEL (app->status)` by četla uvolněnou paměť (UB), ne hlídala.
  Metodická past: první pokusy nic nereprodukovaly, protože skript sám držel
  referenci na okno (únik z `g_list_model_get_item`) a widgety přežily.
- **Oprava** (`src/app/main.c`): `app_teardown()` — ID čtyř zdrojů +
  `g_clear_handle_id`, stop pipeline (join engine vlákna), pak uvolnění
  telnet feedu — zavěšená na `close-request` okna (widgety ještě celé) a jako
  idempotentní pojistka na `shutdown` aplikace; jediný sentinel
  `app->closing` kontrolují ticky, drain fronty událostí, port probe i
  dokončení startu pipeline.
- **Ověřeno:** tatáž reprodukce → 0 CRITICAL, jedna zpráva `app: window
  closed — engine stopped, timers cleared` (oba hooky, jeden teardown),
  zavření → exit 4 ms (odpojený stav); 11 gate zelených.
  **Neověřeno:** zavření s BĚŽÍCÍ pipeline (rádio nešlo použít, mock TCI
  server je uvnitř gate). Ze čtení: `skim_pipeline_stop` joinuje engine
  vlákno před vlastním state callbackem, takže engine nemůže teardown
  předběhnout, a stop z hlavního vlákna už jede v `apply_state` a při změně
  módu. **Příští živé zavření = kontrola: zpráva ano, CRITICAL ne.**
