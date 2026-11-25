//! ASR 引擎装配：按 `asr.engine` 选择实现。
//!
//! `airtype-core` 只内置 `mock` / `command` 两种纯标准库实现；生产用的
//! `sherpa`（内嵌 sherpa-onnx）在 `airtype-asr-sherpa`。core 不能反向依赖
//! native 引擎，所以分发逻辑放在这里，装配完再交给 `Pipeline`。

use std::sync::Arc;

use anyhow::{anyhow, Result};
use airtype_core::{build_engine, build_streaming_engine, AsrEngine, Config, StreamingAsr};

pub struct Engines {
    pub asr: Arc<dyn AsrEngine>,
    /// `None` = 整段模式；`Some` = 流式模式。
    pub streaming: Option<Arc<dyn StreamingAsr>>,
}

pub fn build(cfg: &Config) -> Result<Engines> {
    if cfg.asr.engine == "sherpa" {
        let set = airtype_asr_sherpa::build_engines(
            &cfg.sherpa,
            cfg.base_dir.as_deref(),
            cfg.streaming.enabled,
        )
        .map_err(|e| anyhow!("sherpa 引擎装配失败: {e}"))?;
        return Ok(Engines {
            asr: set.offline,
            streaming: set.streaming,
        });
    }

    Ok(Engines {
        asr: build_engine(&cfg.asr).map_err(|e| anyhow!("ASR 引擎装配失败: {e}"))?,
        streaming: if cfg.streaming.enabled {
            Some(
                build_streaming_engine(&cfg.asr)
                    .map_err(|e| anyhow!("流式引擎装配失败: {e}"))?,
            )
        } else {
            None
        },
    })
}
