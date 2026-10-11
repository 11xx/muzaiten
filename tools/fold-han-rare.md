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

Batch records may come from any predictor. Header identifiers exclude path separators,
relative paths and session identifiers. Source URLs exclude credentials, localhost,
private IP literals and local host suffixes. Sound-component, stated-basis and hint
text exclude path-like tokens and session identifiers. Fixtures may use `fixture:`
source identifiers.
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
result is a tone-marked syllable. The shared syllable admission accepts the
directly attested exceptional CNS spellings `piā`, `yái`, `nuí`, `sēi` and `gué`
unchanged. The nucleusless `q` token is held with its phonetic, Hanyu-column and
Unicode-mapping witnesses; it supplies neither attestation nor a tone.
CNS reader lists follow `CNS_phonetic.txt` order, deduplicating each character's
admitted readings across all its CNS codes. The reading evidence retains archive
and member hashes, line numbers and raw lines, including held tokens.
CNS `ㄇㄡˇ` rows use the pin-specific `cns-mou3/1` rule. A source label beginning
with 常用國字標準字體表, 次常用字國字標準字體表, 罕用國字標準字體表 or 異體國字字表
keeps the reading (22 rows). Rows whose labels are all Unicode imports of basic
CJK, Extension A or Extension B are excluded (3,860 rows), except CNS codes
10-2757, 10-2759, 10-2F49, 10-337B, 10-3A7B, 10-4870, 11-4231 and 11-4555.
Those eight codes and all remaining source classes are quarantined (130 rows).
Excluded and quarantined tokens supply no attestation, tone or family-target
evidence. A character with only a quarantined token has a blocking provenance
finding until independent evidence or a reviewed, dated source settles it.
Excluded rows remain in the evidence ledger. Applying this rule to any CNS
Properties archive other than the pinned 20260805 bytes fails.
Any other reference spelling that is not
tone-marked pinyin attests nothing and is a blocking `provenance` finding naming
the character, the spelling and its source, such as `unparseable cns reading`.

Syllable matching removes the four tone marks and preserves ü versus u and ê
versus e. An exact prediction has priority over a syllable-only match when
choosing its batch; within a match class, the first lexical batch is used.
A toneless guess can obtain several reference tones; it never invents a tone.
Predictions without an attested syllable are withheld and recorded in gaps.
All exact reference readings under the character ship; attestation is not
inherited from a variant's reading.

For `tgh`, the first valid `kTGHZ2013` reading is decisive for the default,
before region and evidence precedence. Every other attested reading remains;
its order follows the general rules. Without a valid `kTGHZ2013` reading those
rules choose the default, and any unparseable source spelling remains a finding.
For other tiers, region class is the first sort key: mainland-attested readings
precede Taiwan-only readings. Within each region, evidence order is both lineages,
then one lineage plus prediction, then unpredicted single-lineage readings.
Ties use exact `kMandarin` order, then
batch/model reading order. Remaining ties use lexical spelling for a reproducible
proposal and require a reviewed decision when they affect the first reading.
Mainland evidence is the first `kMandarin` value, `kHanyuPinyin`, `kXHC1983` or
`kTGHZ2013`. CNS-only readings and a second `kMandarin` value alone provide Taiwan
evidence. A predicted CNS-only reading cannot outrank an unpredicted mainland
reading. Two or more CNS-only readings without a confirmed prediction require
a reviewed default; one attested CNS-only reading needs no choice. A decider
can choose an attested default for either an evidence-order tie or a CNS-only choice
by aligning table order and provenance ranks, then recording the exact resulting
finding in the reviewed rows. A permutation must preserve the region, evidence,
`kMandarin` and prediction precedence of readings outside the tie. Duplicate,
missing and unattested readings cannot settle a choice.
Different prediction passes produce a blocking disagreement finding; they
never count as independent attestation.

Direct families join through `kSimplifiedVariant`, `kTraditionalVariant`,
`kZVariant` and OpenCC `TSCharacters`, including self-containing pairs.
Relation endpoints are NFC-normalized, which maps compatibility ideographs to
their unified counterparts. The reference graph includes Unihan 18's Extension J
even when Python has not assigned its vertices. Output membership uses the
generator's output blocks and runtime categories independently of the graph.
Assembly requires category `Lo` in the build Python's Unicode data. A selected
family target outside that boundary is withheld as an `unsupported-target`
gap naming its relation path; the family and its unresolved decisions remain
intact. Block-based tier members outside that boundary are recorded as
`unsupported-member` gaps naming the Unicode version and category. Complete
proposals cover every buildable member and report every withheld member;
partial proposals report withheld members in their explicit selection.
Run assembly with the same Python Unicode database used to generate the build.
The exact 線–缐 pair is excluded from family joins so 缐 retains its written key;
each excluded source relation remains a `policy-excluded:<relation>` gap record.

The family target comes from the first rule that applies: the family's BMP
simplified members, otherwise its `kTGH` members, otherwise its BMP members.
When that rule names one member, it is the target. Only when all three preference
sets are empty does the family use its unique member directly named as simplified
by `kSimplifiedVariant` or OpenCC. A nonempty ambiguous preference set cannot be
overridden by that fallback. A family with several preferred members, or no
unique fourth-rule target, gives every member a blocking `family-target` finding
listing the candidates of the first nonempty preference set, or all family
members when no preference applies.

A decider chooses a target from that list. Each non-target table row names it in
the simplified column with matching mapping provenance; the target's own column
stays empty. All supplied choices within a family must agree. The checker
recomputes relation paths from that target and retains one exact
`family-target` finding per listed table member, without table/record mismatch
findings for a consistent choice. A reviewed row matches the member, kind,
selected mapping (or `-` for the target itself), and the full
`family-target:unresolved=<candidate list>` reference. For example, selecting 劎
from `劎 劔` keeps that exact reference on every member. Missing provenance,
conflicting choices and targets outside a nonempty preference set cannot be
settled by that family-choice row.

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
python3 tools/fold-han-rare.py assemble --tier core \
  --members selected-members.txt --output proposals
```

Assembly writes `<tier>.tsv`, `<tier>-provenance.tsv`, `<tier>-gaps.tsv` and
`<tier>-findings.tsv`. These are proposals, including when findings are present.
Copy a settled provenance sidecar to the per-tier name when preparing authored
data. Full-tier CLI assembly declares `# complete`. `--members` selects one
ideograph per line from that tier and writes only those members with `# partial`;
predictions outside the selection are rejected. The `write_assembly()` API writes
partial output by default; `complete=True` requires exactly the full tier.
Its `mapping_choices` and `reading_choices` parameters carry explicit choices
without suppressing their findings. Tables declare mainland placement, transcription
attribution and per-reading counts by basis. Every output header carries all
reference source/revision/checksum pins and the checksums of the verbatim
prediction-log files. An `authored` header pins preceding-tier membership by
SHA-256 of sorted ideographs, one per LF-terminated line; its revision is that
content checksum. `--authored` must supply the same preceding-tier membership
for assembly and checking. No runtime-dependent timestamps or filesystem paths enter
generated headers. Preserve the referenced logs with the sidecars.

`assemble --decisions reviewed.tsv` supplies choices for both full and
`--members` proposals. It uses the five-column reviewed-row format:

```text
ideograph<TAB>kind<TAB>ours<TAB>reference<TAB>reason
```

Blank lines and comments are allowed; a header is optional. `family-target`
and `variant-guard` rows put the chosen target ideograph in `ours`.
`provenance` rows put the complete ordered list of attested readings there.
`reference` must be the exact emitted choice finding, including the candidate
list or default-choice text. The reason must be nonempty. Rows for members
outside the selection and other finding kinds supply no choices, so one ledger
can serve several tiers. Conflicting choices, stale findings, unattested
readings and permutations outside the allowed precedence fail before output
is written. Decisions preserve their findings in assembly output; the checker
uses `--reviewed` to discharge their exact keys. A decisions file and explicit
API choice dictionaries cannot be supplied together.

## Expected changes and staged closure

CLI assembly also writes `<tier>-expected-changes.tsv`,
`<tier>-pull-forward.tsv` and `<tier>-impact.json`. `--authored` and
`--generated` supply the current authored and generated tables; the generated
directory defaults to `src/search/fold/unihan`. An authored row overrides both
generated readings and mappings, including an empty simplified cell.

The expected-change columns are `ideograph`, `kind`, `baseline`, `proposed`.
Kinds are `default-changed`, `reading-added`, `reading-removed`,
`mapping-added`, `mapping-removed`, `mapping-retargeted`. Readings are compared
as distinct toneless search spellings, and mappings as terminal search keys
after following every simplification link. The report covers the selected
members; one `reading-added` entry represents one added search spelling.
A changed default requires a reading in both states. Mapping cycles use the
literal `cycle` in place of a terminal key. Removed readings are retained in
the report so a no-loss claim can be checked.

`--assembly DIR` supplies the full proposed rare-tier horizon. Each table is
either `DIR/<tier>.tsv` or `DIR/<tier>/<tier>.tsv`; all eight tables must contain
exactly their buildable tier members. The current proposal overrides its
selected rows in that horizon. Expected changes then compare the baseline
with the final horizon, while pull-forward simulates landing every tier in
file order from the same baseline. Without `--assembly`, expected changes
compare only the current proposal and closure has status `not-evaluated`.

Pull-forward columns are `file`, `ideograph`, `readings`, `simplified`, `reason`.
Each entry carries a later row in its home file, with its tone-marked readings
and target intact. Reasons are `baseline-pair`, `cycle`, or both. Closure
repeats until every repairable baseline-joined pair remains joined and no
repairable cycle remains; rows already pulled by an earlier step are retained
and counted only at that first step. The report supplies proposed rows and
does not write them into authored files or settle their findings.

The JSON records counts per change kind, input content checksums, baseline and
final cycles, and each landing step's pull counts, remaining splits, unexpected
splits and cycles. `permanent_splits` lists baseline pairs the final horizon
itself splits. These require reviewed decisions outside mechanical closure;
an empty unexpected-split list alone is insufficient evidence of no loss.
Cycle lists include mapping sources whose paths enter a cycle. A horizon with
permanent splits or unrepairable cycles remains explicit in the report, and
never receives an implicit approval. Report findings do not alter the assembler's
exit status, which continues to describe its own blocking findings.

```sh
python3 tools/fold-han-rare.py assemble --tier tgh --decisions reviewed.tsv \
  --authored current-han --generated current-unihan \
  --assembly proposals --output reports/tgh
```

The `write_assembly()` API accepts a `baseline` state and an optional
`report_assembly` horizon to emit the same artifacts. States map ideographs to
`(readings, simplified)` pairs; horizon keys are tier names. Supplying neither
keeps the API focused on the four proposal artifacts.

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
relations, counts, source pins and NOTICE attribution. Prediction batches match the
exact tier; `big5-2` batches do not belong to `big5`. Complete-tier coverage is checked
even when the supplied sidecar agrees with a missing table row. Essential tables without
the marker retain their authored-data checks.

Exit status is 0 for no blocking findings, 1 for findings, 2 for invalid command
inputs. Gap records are informational: withheld guesses and guard-failed links cannot ship. Reviewed
decisions use the existing five-column reviewed-row format and suppress only
exact `(ideograph, kind, ours, reference)` findings. A reviewed decision must
retain the factual attestation and prediction evidence; it is not a substitute
for missing logs. The provenance-aware audit supplies the default rule for
marked tables, including reading order, simplified mappings, family targets and
partial-variant guards. The older `archaic`, `simplified`, `family-target` and
single-candidate `variant-guard` rules apply only to unmarked tables. Other
checks, including tone, format, missing readings, coverage and OpenCC
disagreements, still require resolution.

Run the combined offline suite with
`python3 tests/test_fold_han_data.py .`. It includes the records/assembly tests
and requires no model access or reference downloads.
`python3 tests/test_fold_han_rare.py` also assembles every tier from the cached
pinned references, read-only, and skips that check when they are not cached.
A cached file with a different checksum fails the smoke check; it is never downloaded.

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

[NOTICE](../src/search/fold/tables/han/NOTICE) is generated from
[the source-terms registry](fold-han-source-terms.json) and source table pin
headers. Each `# attribution: <JSON object>` record has `reference`, `provider`,
`license`, `pins` and `files`; its rendered statement and file scope must agree
with the registry. Empty catalog scopes attribute no distributed table.

Every input pin of a marked table needs known terms and a scoped attribution,
including sources used for membership or prediction attestation. Synthetic
input snapshots support assembly experiments; an unregistered exact pin fails
source-term checking. The production CLI has no registry override. Unihan uses
Unicode-3.0 with its pinned archive notice and complete license text. CNS uses
OGDL-Taiwan-1.0 with agency/year/full title/version, the conditional-use
statement, license URL and provider site credit. OpenCC uses its pinned
Apache-2.0 dictionary terms and exact license copy. A prominent modified-file
notice appears in the table and sidecar if and only if they contain
OpenCC-attested mappings.

Missing terms, attribution scope, legal copies or modified-content notices
produce `source-terms` findings. Reviewed reading or mapping decisions cannot
suppress these findings. Full OGDL text and OpenCC AUTHORS are conservative
project-policy copies; the checker distinguishes them from license-required
copies. Source and binary installation preserve NOTICE and the copies through
CMake's `HanNotices` component.

```sh
python3 tools/fold-han-notice.py render --write
python3 tools/fold-han-notice.py check
python3 tests/test_fold_han_notice.py
```

Adding row-bearing tiers or changing a source pin requires updating the exact
terms, regenerating NOTICE, and synchronizing package license metadata with the
actual data components. Basis labels describe how a value was obtained; they
do not decide copyright, database rights, corpus compatibility or a release
license. Decomposition inputs require their own source-license determination;
prediction metadata and a blind method do not confer distribution rights.
