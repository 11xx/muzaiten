use std::path::PathBuf;

use anyhow::Result;
use clap::{Parser, Subcommand, ValueEnum};
use muzaiten_index::{ScanOptions, Stage, scan, status_json};

#[derive(Debug, Parser)]
#[command(name = "muzaiten-index")]
#[command(about = "Build muzaiten sidecar audio feature indexes")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Debug, Subcommand)]
enum Command {
    Scan {
        #[arg(long)]
        library: PathBuf,
        #[arg(long)]
        features: PathBuf,
        #[arg(long, value_enum, default_value_t = StageArg::All)]
        stage: StageArg,
        #[arg(long)]
        limit: Option<usize>,
        #[arg(long)]
        jobs: Option<usize>,
    },
    Status {
        #[arg(long)]
        features: PathBuf,
    },
}

#[derive(Clone, Copy, Debug, ValueEnum)]
enum StageArg {
    Identity,
    Features,
    All,
}

impl From<StageArg> for Stage {
    fn from(value: StageArg) -> Self {
        match value {
            StageArg::Identity => Stage::Identity,
            StageArg::Features => Stage::Features,
            StageArg::All => Stage::All,
        }
    }
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    match cli.command {
        Command::Scan {
            library,
            features,
            stage,
            limit,
            jobs,
        } => {
            let summary = scan(ScanOptions {
                library,
                features,
                stage: stage.into(),
                limit,
                jobs,
            })?;
            println!("{}", serde_json::to_string(&summary)?);
        }
        Command::Status { features } => {
            println!("{}", serde_json::to_string(&status_json(&features)?)?);
        }
    }
    Ok(())
}
