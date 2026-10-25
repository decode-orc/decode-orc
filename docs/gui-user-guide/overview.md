# Overview

Before using **decode-orc**, it is helpful to understand a small number of core concepts that appear throughout the application and documentation. These concepts describe *how decoding work is structured*, *how data flows*, and *how execution is observed*.

![](../assets/orc-gui-screenshot.png)

---

## Directed Acyclic Graph (DAG)

At the highest level, a decode-orc decoding pipeline is represented as a **Directed Acyclic Graph (DAG)**.

### What this means

- **Directed**  
  Data flows in one direction only, from inputs to outputs.

- **Acyclic**  
  There are no loops. A stage can never depend on its own output, directly or indirectly.

- **Graph**  
  The pipeline is made up of *stages* and *connections*.

### Why decode-orc uses a DAG

Using a DAG provides several important guarantees:

- A clear and deterministic execution order
- Prevention of invalid or circular pipelines
- Easier inspection, validation, and debugging of pipelines

You can think of the DAG as a **blueprint** for how decoding will happen before anything actually runs.

---

## Stages

A **stage** is the basic unit of work in decode-orc.

Every stage:

- Receives zero or more inputs
- Produces zero or more outputs
- Performs one well-defined task
- Connects to other stages to form the DAG

Stages do *not* decide when they run. They simply declare:

- What data they need
- What data they produce

The orchestration system uses this information to execute the DAG correctly.

---

## Types of Stages

### Source Stages

**Source stages** introduce data into the pipeline.

#### Characteristics

- Have **no inputs**
- Have **one or more outputs**
- Act as the starting points of the DAG

#### Typical responsibilities

- Reading TBC video files
- Reading metadata (for example from SQLite databases)
- Emitting audio, timing, or configuration data

If you imagine the pipeline as a factory, source stages are the **raw material loaders**.

---

### Transform Stages

**Transform stages** take input data, process it, and emit new data.

#### Characteristics

- Have **one or more inputs**
- Have **one or more outputs**
- Perform deterministic transformations

#### Typical responsibilities

- Video or signal processing
- Frame decoding
- Color space conversion
- Timing or synchronization adjustments

Transform stages do not write files and do not observe results. They simply transform data and pass it on.

These stages form the **core processing chain** of the pipeline.

---

### Sink Stages

**Sink stages** consume data and produce final outputs targeted to the associated ld-decode/vhs-decode project tools, or to external tools such as the Daphne arcade LaserDisc emulator.

#### Characteristics

- Have **one or more inputs**
- Have **no outputs**
- Perform side effects

#### Typical responsibilities

- Writing decoded video files
- Writing audio output
- Exporting metadata or intermediate artifacts

Sink stages mark the **end points** of the DAG.

---

### Analysis Sink Stages

**Analysis sink stages** are a special type of sink used for *analysis and evaluation of decode quality*.

In decode-orc, transform stages may improve or degrade the quality of the data as it flows through the DAG.  Because of this, it is important to be able to measure and analyse the data **at multiple points across the pipeline**, not only at the final output.

Analysis sink stages exist to support this requirement.

---

#### How they differ from normal sinks

- They do **not** affect the decoding result
- They do **not** produce final output artefacts
- They exist purely to analyse data at a specific point in the DAG
- They are optional and can be used as needed

---

#### Typical uses

- Measuring signal or video quality
- Evaluating the effect of transform stages
- Comparing quality before and after processing stages
- Generating metrics, statistics, or reports describing decode quality

---

You can add or remove analysis sink stages without changing the meaning or correctness of the decode pipeline.

Their purpose is to provide **objective insight into the quality of the decoding process**, rather than to assist with debugging or development.

---

## Hints

In decode-orc, **hints** are pieces of information that are *ingested by source stages* and made available to the rest of the pipeline.

Hints represent information that **cannot be reliably derived by observing the decoded signal alone**.

---

### What hints are

- Hints are metadata associated with the data flowing through the pipeline
- They originate primarily from **source stages**
- They describe properties of the input material or capture process

Typical examples include:

- Video parameters (standards, formats, dimensions)
- Timing information supplied by capture tools
- Known dropouts or damaged regions
- Metadata extracted from external sources (for example decode databases)

This information is known *at ingest time*, not discovered during processing.

---

### Why hints exist

Some information is fundamentally unavailable through observation alone.

For example:

- A dropout that occurred during RF capture
- The intended video standard or field structure
- Capture-time metadata stored in a database
- External measurements or annotations

Observers can only see what the pipeline produces.  
Hints allow decode-orc to incorporate **external knowledge** into the decode process.

---

### How hints are used

- Source stages attach hints to the data they emit
- Downstream stages may consult hints to:
  - Adjust processing behaviour
  - Handle known problem areas
  - Interpret data correctly
- Stages are free to ignore hints if they are not relevant

Hints inform *how* decoding happens, not *what* data flows through the DAG.

---

### Important properties

- Hints are **informational**, not computational
- They do **not** replace signal analysis
- They do **not** act as control flow
- They are not generated by observers

A correct decode pipeline should still function if hints are absent, but may produce lower-quality or less-informed results.

---

### How hints differ from observers

- **Hints** provide *prior knowledge* supplied at ingest time
- **Observers** provide *insight* into data during execution

Hints answer:  
> “What do we already know about this material?”

Observers answer:  
> “What does the pipeline produce as it runs?”

---

## Observers

In decode-orc, **observers** are used to *inspect data flowing through the pipeline* without changing the meaning of the decode.

Observers are *non-intrusive*: they observe data rather than transform or terminate it. They are not stages, and you do not add them to the graph — they run automatically on the output of whichever stage you are looking at.

---

### What observers are

- Observers read the output of a stage — the same data that flows on to the next stage
- They exist to *observe*, *measure*, or *report* on that data
- Each observer specialises in one kind of information, such as LaserDisc VBI codes, closed captions, colour burst level or signal-to-noise ratio (see [Observers](observers/overview.md))

Conceptually, an observer is similar to a tap placed on a cable:  
it sees the signal, but does not alter it.

---

### What observers do

- Inspect data produced by stages
- Collect statistics or measurements
- Perform validation or sanity checks
- Feed the Preview window's **Observers** windows, and stage tools such as the Disc Mapper

In the GUI, observations are computed in the background for the stage being previewed and kept, so stepping back to a frame already seen is immediate. The status bar reports the work while it runs.

---

### What observers do *not* do

- They do **not** modify data
- They do **not** feed data back into the pipeline
- They do **not** influence decoding results
- They do **not** replace sink or transform stages

Their presence never changes the decode output.

---

### How observers relate to the DAG

- Observers are attached to a stage's output, not wired into the graph
- They depend on the stages upstream of that output for their input
- Nothing downstream depends on them

This ensures:

- Deterministic observation
- No hidden side effects

---

### Why observers exist

Observers allow decode-orc to answer questions such as:

- *Is this signal within expected limits?*
- *How noisy is this section of the decode?*
- *Are timing assumptions being violated?*
- *What does the intermediate data actually look like?*

All without altering the decode pipeline itself.

---

## How These Concepts Fit Together

A typical decode-orc workflow looks like this:

1. **Stages** are created and connected
2. The connections form a **Directed Acyclic Graph**
3. **Source stages** inject data into the pipeline
4. **Transform stages** process and pass data along
5. **Sink stages** write final outputs
6. **Analysis sinks** optionally measure results at chosen points
7. **Hints** flow alongside data where useful
8. **Observers** report on the output of the stage being inspected

---

## A Simple Mental Model

Think of decode-orc as:

> A data-processing factory, where each machine (stage) performs one job, conveyor belts (edges) move data forward, labels (hints) provide optional context, and cameras (observers) watch everything happen.

Understanding these concepts will make it much easier to configure pipelines and reason about how decoding work is performed.

Now you are ready to read through the other sections of this user guide, which look at each of these concepts in more detail:

- [Quick Projects](quick-projects.md) — the fastest way to get a capture into a working pipeline
- [Main window](dialogues/main.md) — menus, the graph editor, stage tools and the Plugin Manager
- [Preview](dialogues/preview.md), [Frame Scope](dialogues/line-scope.md), [Frame Timing](dialogues/field-timing.md) and the [Dropout Editor](dialogues/dropout-editor.md)
- [Stages](stages/overview.md) — what every stage does and its parameters
- [Observers](observers/overview.md) — what each observer measures
- [Using Plugins](../plugins.md) — installing and updating third-party stages
