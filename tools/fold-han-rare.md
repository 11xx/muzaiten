# Rare Han records and assembly

`fold-han-rare.py` assembles proposals from blind prediction logs and checksum
pinned Unihan, CNS 11643 and OpenCC inputs. It launches no models. Its output
uses the authored three-column format and the membership/order returned by
`fold-han-data.py`'s `tiers()`. Provenance and logs are developer artifacts;
the build reads only the authored character tables.

## Prediction logs

Store immutable batches in `tools/fold-han-predictions/<tier>-<batch>.tsv`.
The headers are:

```text
# schema: fold-han-predictions/2
# metadata: <JSON object>
# columns: ideograph<TAB>readings<TAB>confidence<TAB>sound_component<TAB>stated_basis<TAB>decomposition_sha256<TAB>hint<TAB>normalized_readings<TAB>simplified
```

`<TAB>` denotes a literal tab. Metadata has exactly these keys:

| Key | Value |
| --- | --- |
| `batch` | Unique filename-safe identifier; lexical order settles ties between batches |
| `model`, `effort` | Portable predictor coordinates, or `unknown`; mechanical predictors use their own identifier |
| `date` | ISO date of generation |
| `prompt_sha256` | SHA-256 of the exact prompt bytes |
| `decomposition` | Source pointer with `id`, `source` (public URL), pinned `revision`, `sha256` |
| `hint` | Object with `source` (public URL), `revision`, `sha256` |

Rows preserve the predictor's raw reading spelling and order. `normalized_readings`
contains the same list in NFC. Both lists use `-` when there is no guess.
Confidence is `c` (recalled), `g` (guessed) or `unknown`; it never attests a
reading. The model's stated basis is descriptive text, independent of the
classified basis. `decomposition_sha256` hashes the exact UTF-8 input bytes
without normalization or trimming; `-` means no decomposition was supplied.
The source pointer identifies the dataset and pinned revision; its `sha256`
pins the source snapshot, independently of each row's input hash. Verbatim
decomposition text has no column. Descriptive fields must not reproduce supplied
decomposition strings or other third-party text. The hint records the authored
input supplied; an empty cell means none. A blind mapping guess occupies `simplified`, otherwise
that cell is empty. Duplicate characters within a batch, duplicate batch IDs,
malformed pinyin, mismatched normalization, missing metadata and extra columns
are errors. Text-bearing schema 1 and unversioned five-column logs are not accepted
as shipping prediction logs.

Batch records may come from any predictor. Header coordinates exclude local
paths and session identifiers. Fixtures may use `fixture:` source identifiers.
Keep verbatim decomposition inputs, prompts and raw responses in private evidence
until their source terms permit distribution. Converting responses into this
format must preserve their checksums and conversion evidence outside the shipping
tables. Source pointers and hashes do not establish a public-domain status.

## Provenance and ranking

Store sidecars in `tools/fold-han-provenance/<tier>.tsv`:

```text
# schema: fold-han-provenance/1
# references: <reference manifest JSON>
# inputs: [{"batch":"example-1","sha256":"<64 lowercase hex digits>"}]
# columns: ideograph<TAB>value<TAB>basis<TAB>attested_by<TAB>tone_from<TAB>rank<TAB>batch
```

One record describes each shipped reading or mapping. Reading ranks start at
one; rank zero denotes a simplified mapping. Reading attestation is a sorted,
comma-separated set of `cns` and `unihan`. Several Unihan fields are one lineage.
Agreement requires the same NFC reading including tone in both lineages;
syllable-only agreement cannot attest a tone.

| Basis | Meaning | `tone_from` | `batch` |
| --- | --- | --- | --- |
| `predicted` | A blind guess matches the exact reading | `-` | Matching batch |
| `predicted` | A blind guess matches only its syllable (`pred-syllable`) | Attesting lineages | Matching batch |
| `agreed` | No prediction matches; both lineages attest the exact reading | `cns,unihan` | `-` |
| `single` | No prediction matches; one lineage attests the exact reading | That lineage | `-` |

Reference readings are NFC tone-marked pinyin. CNS writes a third tone after a
vowel with the spacing caron (U+02C7); it reads as the combining caron when the
result is a tone-marked syllable. Any other reference spelling that is not
tone-marked pinyin attests nothing and is a blocking `provenance` finding naming
the character, the spelling and its source, such as `unparseable cns reading`.

Syllable matching removes the four tone marks and preserves ü versus u and ê
versus e. An exact prediction has priority over a syllable-only match when
choosing its batch; within a match class, the first lexical batch is used.
A toneless guess can obtain several reference tones; it never invents a tone.
Predictions without an attested syllable are withheld and recorded in gaps.
All exact reference readings under the character ship; attestation is not
inherited from a variant's reading.

Region class is the first sort key: mainland-attested readings precede Taiwan-only
readings. Within each region, evidence order is both lineages, then one lineage
plus prediction, then unpredicted single-lineage readings. Ties use exact `kMandarin` order, then
batch/model reading order. Remaining ties use lexical spelling for a reproducible
proposal and require a reviewed decision when they affect the first reading.
Mainland evidence is the first `kMandarin` value, `kHanyuPinyin`, `kXHC1983` or
`kTGHZ2013`. CNS-only readings and a second `kMandarin` value alone provide Taiwan
evidence. A predicted CNS-only reading cannot outrank an unpredicted mainland
reading. Two or more CNS-only readings without a confirmed prediction require
a reviewed default; one attested CNS-only reading needs no choice. A decider
can choose any attested default in that case by aligning table order and provenance
ranks, then recording the exact resulting default finding in the reviewed rows.
Different prediction passes produce a blocking disagreement finding; they
never count as independent attestation.

Direct families join through `kSimplifiedVariant`, `kTraditionalVariant`,
`kZVariant` and OpenCC `TSCharacters`, including self-containing pairs.
Relation endpoints are NFC-normalized, which maps compatibility ideographs to
their unified counterparts; a relation naming any other character outside the
checker's ideograph blocks joins nothing. Tier membership lies within the same
blocks.

The family target comes from the first rule that applies: the family's BMP
simplified members, otherwise its `kTGH` members, otherwise its BMP members.
When that rule names one member, it is the target. When it names several, or no
rule applies (an all-astral family without a `kTGH` member), the target is the
family's only member named as a simplified form by `kSimplifiedVariant` or OpenCC.
A family with none or several such members gives every member a blocking
`family-target` finding.

Preceding tiers establish authored/typed membership; the current tier and later
tiers cannot change that baseline during an audit. Partial semantic links
require an unauthored/non-`kTGH` source, an authored/`kTGH` target and a shared
syllable. A link failing that guard joins nothing; it is recorded as a gap with
reason `guard-failed:<field>`. When several candidates pass, their score counts
independent lineages that attest a shared syllable at both source and target:
Unihan and CNS each count once, regardless of the number of Unihan fields. The
unique highest score wins. Tied candidates from one direct family join without
a choice, since each gives the same target. A tie across families produces a
blocking `variant-guard` finding listing each candidate and score, with
`candidate→target` where the candidate's direct family target differs, such as
`variant-guard:tied-lineages=櫓→橹:1 禾:1`. A decider settles it by writing the
family target of one tied candidate (`橹`, not `櫓`) in the table's simplified
column and recording its mapping provenance; the checker still emits the exact
tied-choice finding until a reviewed row settles it. `kSpoofingVariant` is
excluded. Mapping attestation names the relations along
a deterministic shortest path, such as `unihan:kZVariant,opencc:TSCharacters`
(sorted lexically). Unihan and OpenCC do not establish independent agreement;
mapping basis is `single`, or `predicted` for an exact blind mapping guess.
Mapping `tone_from` is `-`. MOE relations require an input adapter and pinned
format before they can participate.

## Commands and headers

```sh
python3 tools/fold-han-rare.py assemble --tier extb \
  --predictions tools/fold-han-predictions/extb-example-1.tsv \
  --output proposals
python3 tools/fold-han-rare.py check proposals/extb.tsv \
  --provenance proposals/extb-provenance.tsv \
  --predictions tools/fold-han-predictions/extb-example-1.tsv
```

Assembly writes `<tier>.tsv`, `<tier>-provenance.tsv`, `<tier>-gaps.tsv` and
`<tier>-findings.tsv`. These are proposals, including when findings are present.
Copy a settled provenance sidecar to the per-tier name when preparing authored
data. The table declares `# complete`, mainland placement, transcription
attribution and per-reading counts by basis. Every output header carries all
reference source/revision/checksum pins and the checksums of the verbatim
prediction-log files. An `authored` header pins preceding-tier membership by
SHA-256 of sorted ideographs, one per LF-terminated line; its revision is that
content checksum. `--authored` must supply the same preceding-tier membership
for assembly and checking. No runtime-dependent timestamps or filesystem paths enter
generated headers. Preserve the referenced logs with the sidecars.

The default pins come from the shipped checker. Offline fixtures or explicit
reference snapshots use `--references manifest.json` together with all four
files: `--unihan`, `--cns-properties`, `--cns-mappings`, `--opencc`. The manifest
has schema `fold-han-references/1` and keys `unihan`, `cns-properties`,
`cns-mappings`, `opencc`, each containing `source`, `revision`, `sha256`.
All four file checksums are verified before parsing their shipped formats.

`fold-han-data.py check` requires provenance for rare tiers and for essential
tables bearing the provenance schema header. `--provenance DIR` and
`--predictions DIR` select their artifact directories. The provenance audit
recomputes basis, attestation, tones, batch evidence, reading order, mapping
relations, counts and source pins. Complete-tier coverage is checked even when
the supplied sidecar agrees with a missing table row. Essential tables without
the marker retain their authored-data checks.

Exit status is 0 for no blocking findings, 1 for findings, 2 for invalid command
inputs. Gap records are informational: withheld guesses and guard-failed links cannot ship. Reviewed
decisions use the existing five-column reviewed-row format and suppress only
exact `(ideograph, kind, ours, reference)` findings. A reviewed decision must
retain the factual attestation and prediction evidence; it is not a substitute
for missing logs. The provenance-aware audit supplies the default rule for
marked tables; the other authored-data findings still require resolution.

Run the combined offline suite with
`python3 tests/test_fold_han_data.py .`. It includes the records/assembly tests
and requires no model access or reference downloads.
`python3 tests/test_fold_han_rare.py` also assembles every tier from the cached
pinned references, read-only, and skips that check when they are not cached.

## Dispatch API proposal

A dispatch runner needs a separately authorized contract. A proposed interface
is `prepare --tier T --pass P --batch-size N --output DIR`, then
`collect --manifest FILE --responses DIR --output DIR`. Preparation would emit
an immutable manifest containing membership order, predictor route coordinates,
prompt/decomposition/hint hashes and permitted output filenames. Generation
briefs must expose only membership, decompositions and authored hints, keeping
reference views in a separate checker workspace. Collection would validate
response identity, complete batch membership and the records above, retain raw
response bytes/checksums, and refuse partial or duplicate batches. Reference
checks would be separate jobs with their own manifests and exact finding keys.
Launch authority, route selection, concurrency, budget, retry/cancellation rules,
output ownership and signing must be established before adding execution.

## Source notices

[NOTICE](../src/search/fold/tables/han/NOTICE) records the reference providers
and terms. Basis labels describe how a value was obtained; they do not decide
copyright, database rights, license compatibility or the release license.
Transcribed-file inventories and the distribution license require a release
decision based on the actual assembled corpus and retained evidence. CNS source
coordinates are reference metadata; distribution attribution text accompanies
the first shipped CNS-derived rows after the applicable license decision.
Decomposition inputs require their own source-license determination; prediction
metadata and a blind generation method do not confer permission to distribute
them. Synthetic fixtures exercise the format without embedding dictionary
decompositions or model output.
