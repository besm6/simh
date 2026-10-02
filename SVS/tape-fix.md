The bug was added in the СВС port. In the BESM-6 version (~/git/re-dispak/v1k.be), characters from the standard passport don't advance СЧСИМ, so the passport can't throw the tape's card count off. The ФОРМЕ/НАЧАЛ split I described is identical in both versions; only the СВС version is affected by it.

How BESM-6 does it (v1k.be:237-264):
- ЗАСИМВ with Е15 set takes the next character from the passport buffer, puts it in R, and goes straight to ВЫБ6.
- That path never reaches ВЫБ3А, where СЧСИМ is incremented. Tape position (СТВУ, РЯВЫБ, M4) and СЧСИМ stay together, and only M4 needs saving, which the code already does.
- СЧСИМК is only an alias of ЧИСЛСИ (эквив (числси,счсимк), v1k.be:1141). The save in В3В4 is commented out (v3v4.be:381-382: * СЧ СЧСИМ / * ЗП СЧСИМК).

What the СВС port changed. The re-dispak source marks the spots with свс: comments:
- ЗП R became ЗП СИМВОЛ, and ПЕ ВЫБ6 became ПЕ ДАЙСИ2 (v1k.be:255,257). Passport characters now go through the formation path ДАЙСИ1/ФОРСИ and then ВЫБ3А.
- That path lets a standard passport use 0341 (pad the card) and six 0342 (end of А3). The cost is that every passport character now advances СЧСИМ and the card count.
- To compensate, the port saves СЧСИМК in В3В4 (в3в4:388-389, active again) and restores СЧСИМ from it when the passport runs out (в1к:499-500). That covers only a passport read in one go. ФОРМЕ and НАЧАЛ, which are unchanged from BESM-6, don't save or restore it, so the count ends up wrong whenever the passport is split at its Е.

So the fix I proposed earlier is correct, and there's a smaller alternative: do the save in НАЧАЛ and the restore in ФОРМЕ, or send only the passport's end-of-text back through ВЫБ6 the BESM-6 way. The cheapest option is to restore СЧСИМ from СЧСИМК in ФОРМЕ and re-save it in НАЧАЛ.

Unchanged from BESM-6:
- The А3А test of СТПАСП & Е15 (v6v7.be:1240) is the same, so that latent card-only bug is original.
- СИПКПЛ differs: at the end of each card BESM-6 zeroes the 20 words it has used and keeps its own card count, while СВС refills the whole 960-frame buffer through ДАЙБУФ. This doesn't affect the passport problem.

On the earlier ФС5 question: BESM-6 В3В4 does set the five-track flag: ВОСЬМИ := НУСВУ & Е4 (v3v4.be:477-479), so bit 4 of the device number picks eight-track versus five-track. The СВС в3в4.bemsh dropped that store but kept the ВНЕШ ВОСЬМИ declaration. That is why ВОСЬМИ stays 1 and the МТК-2 decoder can't be reached.
