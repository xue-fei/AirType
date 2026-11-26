//! 文本后处理：口头词、空白、全角字符、末尾标点。
//! 纯字符串处理，不依赖任何平台 API。

use crate::config::PostProcessConfig;

const FILLERS: &[&str] = &["嗯", "啊", "呃", "这个", "那个", "然后", "就是说", "就是"];
/// 只有语气词、没有任何实义内容时，整段视为无语音。
const NON_SUBSTANTIVE: &[char] = &['嗯', '啊', '呃', '哦', '唔', '呵'];
const END_PUNCT: &[char] = &[
    '。', '，', '、', '；', '：', '？', '！', '…', '”', '’', '）', ']', '}', '.', ',',
    ';', ':', '?', '!', ')',
];

pub struct PostProcessor {
    cfg: PostProcessConfig,
}

impl PostProcessor {
    pub fn new(cfg: PostProcessConfig) -> Self {
        Self { cfg }
    }

    pub fn process(&self, text: &str) -> String {
        self.process_inner(text, true)
    }

    /// 流式中间结果专用：与 `process` 一致，但**不补句末标点**。
    /// 中文流式识别的 partial 会反复改写，补标点会让末尾多一个字再被删掉，
    /// 造成输入框里标点闪烁。标点只在最终文本上补。
    pub fn process_partial(&self, text: &str) -> String {
        self.process_inner(text, false)
    }

    fn process_inner(&self, text: &str, ensure_punct: bool) -> String {
        let mut s = text.trim().to_string();
        if s.is_empty() {
            return s;
        }
        if self.cfg.collapse_whitespace {
            s = collapse_whitespace(&s);
        }
        if self.cfg.trim_filler_words {
            s = trim_fillers(&s);
        }
        if self.cfg.normalize_numbers {
            s = normalize_fullwidth(&s);
        }
        if self.cfg.normalize_english_case {
            s = normalize_english_case(&s);
        }
        if self.cfg.space_cjk_latin {
            s = space_cjk_latin(&s);
        }
        if self.cfg.collapse_whitespace {
            s = remove_cjk_spaces(&s);
        }
        if ensure_punct && self.cfg.ensure_punctuation {
            s = ensure_punctuation(&s);
        }
        let s = s.trim();
        // 真实 ASR（SenseVoice）对静音/纯音段会返回 "。" / "嗯。" 之类，
        // 注入后只会在输入框留下垃圾标点。没有任何实义字符就丢掉整段。
        if !has_substantive(s) {
            return String::new();
        }
        s.to_string()
    }
}

/// 至少一个字母 / 数字 / 表意文字，且不全是语气词。
fn has_substantive(s: &str) -> bool {
    s.chars()
        .any(|c| c.is_alphanumeric() && !NON_SUBSTANTIVE.contains(&c))
}

fn collapse_whitespace(s: &str) -> String {
    s.split_whitespace().collect::<Vec<_>>().join(" ")
}

fn trim_fillers(input: &str) -> String {
    let mut s = input.trim_start().to_string();
    loop {
        let mut changed = false;
        for f in FILLERS {
            if let Some(rest) = s.strip_prefix(f) {
                let rest =
                    rest.trim_start_matches(|c: char| c == ',' || c == '，' || c.is_whitespace());
                if !rest.is_empty() {
                    s = rest.to_string();
                    changed = true;
                    break;
                }
            }
        }
        if !changed {
            return s;
        }
    }
}

/// 全角数字与全角字母转半角；中文标点保留（中文输入需要它们）。
/// 中文数字（"一二三"）不做转换，避免误改语义。
/// 把 ASR 输出里"全大写的英文词"还原成正常大小写。
///
/// 为什么需要：zipformer 双语模型的英文词表本身是大写（LIBRI 风格），
/// 于是 `Today is` 出来是 `TODAY IS`，`Monday` 出来是 `MONDAY`，整段像在喊。
///
/// 规则：只处理**长度 ≥ 2 且全大写**的纯字母词，转成"首字母大写 + 其余小写"；
/// 单字母 `I` / `A` 和本来就是混合大小写的词（`iPhone`、`Monday`）原样保留。
/// 不做句子级断句——识别结果没有标点，猜句子边界只会猜错。
fn normalize_english_case(input: &str) -> String {
    let mut out = String::with_capacity(input.len());
    let mut word = String::new();

    fn flush(word: &mut String, out: &mut String) {
        if word.is_empty() {
            return;
        }
        let all_alpha = word.chars().all(|c| c.is_ascii_alphabetic());
        let all_upper = word.chars().all(|c| !c.is_ascii_lowercase());
        if all_alpha && word.len() >= 2 && all_upper {
            let mut chars = word.chars();
            if let Some(first) = chars.next() {
                out.push(first); // 本来已是大写，直接保留
                out.extend(chars.flat_map(|c| c.to_lowercase()));
            }
        } else {
            // 混合大小写（iPhone）、单字母（I/A）、含数字的词一律原样保留。
            out.push_str(word);
        }
        word.clear();
    }

    for ch in input.chars() {
        // 数字也算词内字符：否则 "MP3" 会被拆成 "MP" + "3"，
        // "MP" 又被当成全大写词转成 "Mp"，结果 "Mp3"。
        if ch.is_ascii_alphanumeric() {
            word.push(ch);
        } else {
            flush(&mut word, &mut out);
            out.push(ch);
        }
    }
    flush(&mut word, &mut out);
    out
}

fn normalize_fullwidth(s: &str) -> String {
    s.chars()
        .map(|c| match c {
            '\u{FF10}'..='\u{FF19}' => char::from((c as u32 - 0xFF10) as u8 + b'0'),
            '\u{FF21}'..='\u{FF3A}' => char::from((c as u32 - 0xFF21) as u8 + b'A'),
            '\u{FF41}'..='\u{FF5A}' => char::from((c as u32 - 0xFF41) as u8 + b'a'),
            '\u{FF05}' => '%',
            _ => c,
        })
        .collect()
}

fn is_cjk(c: char) -> bool {
    ('\u{4E00}'..='\u{9FFF}').contains(&c)
        || ('\u{3400}'..='\u{4DBF}').contains(&c)
        || ('\u{3000}'..='\u{303F}').contains(&c)
}

/// 在中文与英文单词之间补一个空格（`Is礼拜二` → `Is 礼拜二`）。
///
/// 只认「含字母的拉丁词」，所以 `3月`、`第2种`、`2026年` 这类数字+中文
/// 保持原样（GB/T 15834 里数字与中文之间不加空格），而 `MP3音乐` →
/// `MP3 音乐`（词里有字母，算中英混排）。
fn space_cjk_latin(s: &str) -> String {
    let chars: Vec<char> = s.chars().collect();
    let mut out = String::with_capacity(s.len() + 8);
    for (i, c) in chars.iter().enumerate() {
        if i > 0 && *c != ' ' {
            let prev = chars[i - 1];
            if prev != ' ' {
                let need = (is_cjk(prev) && c.is_ascii_alphabetic())
                    || (is_cjk(*c) && latin_word_has_alpha(&chars, i));
                if need {
                    out.push(' ');
                }
            }
        }
        out.push(*c);
    }
    out
}

/// `chars[..end]` 结尾那个拉丁词（ASCII 字母数字串）里有没有字母。
/// 用来区分 `MP3`（该加空格）和 `3`（纯数字，不加）。
fn latin_word_has_alpha(chars: &[char], end: usize) -> bool {
    let mut i = end;
    while i > 0 && chars[i - 1].is_ascii_alphanumeric() {
        i -= 1;
    }
    chars[i..end].iter().any(|c| c.is_ascii_alphabetic())
}

/// 中文 ASR 结果里偶发字间空格，删掉夹在两个 CJK 字符之间的空格。
fn remove_cjk_spaces(s: &str) -> String {
    let chars: Vec<char> = s.chars().collect();
    let mut out = String::with_capacity(s.len());
    for (i, c) in chars.iter().enumerate() {
        if c.is_whitespace() {
            let prev = chars[..i].iter().rev().find(|x| !x.is_whitespace());
            let next = chars[i + 1..].iter().find(|x| !x.is_whitespace());
            if let (Some(p), Some(n)) = (prev, next) {
                if is_cjk(*p) && is_cjk(*n) {
                    continue;
                }
            }
            out.push(' ');
        } else {
            out.push(*c);
        }
    }
    out
}

fn ensure_punctuation(s: &str) -> String {
    let trimmed = s.trim_end();
    if trimmed.is_empty() {
        return String::new();
    }
    let last = trimmed.chars().last().unwrap();
    if END_PUNCT.contains(&last) {
        return trimmed.to_string();
    }
    let has_cjk = trimmed.chars().any(is_cjk);
    format!("{}{}", trimmed, if has_cjk { "。" } else { "." })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn default_pp() -> PostProcessor {
        PostProcessor::new(PostProcessConfig::default())
    }

    #[test]
    fn strips_leading_fillers() {
        assert_eq!(default_pp().process("嗯，那个 今天天气不错"), "今天天气不错。");
    }

    #[test]
    fn removes_spaces_between_cjk() {
        assert_eq!(default_pp().process("你 好 世 界"), "你好世界。");
    }

    #[test]
    fn keeps_latin_word_spaces() {
        assert_eq!(default_pp().process("hello world"), "hello world.");
    }

    #[test]
    fn normalizes_fullwidth_digits() {
        assert_eq!(default_pp().process("房间号 ３０２"), "房间号 302。");
    }

    #[test]
    fn does_not_duplicate_punctuation() {
        assert_eq!(default_pp().process("真的吗？"), "真的吗？");
    }

    #[test]
    fn respects_disabled_flags() {
        let cfg = PostProcessConfig {
            trim_filler_words: false,
            normalize_numbers: false,
            ensure_punctuation: false,
            collapse_whitespace: false,
            normalize_english_case: false,
            space_cjk_latin: false,
        };
        assert_eq!(PostProcessor::new(cfg).process("嗯 你好"), "嗯 你好");
    }

    #[test]
    fn empty_input_stays_empty() {
        assert_eq!(default_pp().process("   "), "");
    }

    #[test]
    fn drops_punctuation_and_filler_only_output() {
        // 真实 ASR 对静音段常见的输出，不能注入到输入框
        assert_eq!(default_pp().process("。"), "");
        assert_eq!(default_pp().process("…"), "");
        assert_eq!(default_pp().process("嗯。"), "");
        assert_eq!(default_pp().process("?"), "");
    }

    #[test]
    fn keeps_real_asr_text() {
        assert_eq!(
            default_pp().process("开饭时间早上9点至下午5点。"),
            "开饭时间早上9点至下午5点。"
        );
    }

    #[test]
    fn restores_english_case() {
        // zipformer 的英文词表是大写，出来是 TODAY IS，要还原成 Today Is。
        assert_eq!(
            normalize_english_case("昨天是 MONDAY TODAY IS LIBR"),
            "昨天是 Monday Today Is Libr"
        );
        // 单字母 I / A 不该被小写，混合大小写的词也不动。
        assert_eq!(normalize_english_case("I am A iPhone"), "I am A iPhone");
        // 含数字的词原样保留，别把 ES 之外的东西搅乱。
        assert_eq!(normalize_english_case("MP3 WAV"), "MP3 Wav");
    }

    #[test]
    fn spaces_between_cjk_and_latin_words() {
        // ASR 常把中英挤在一起：Is礼拜二 / Mon的
        assert_eq!(space_cjk_latin("Is礼拜二 The"), "Is 礼拜二 The");
        assert_eq!(space_cjk_latin("Mon的礼拜二"), "Mon 的礼拜二");
        // 已有空格不重复插
        assert_eq!(space_cjk_latin("Is 礼拜二"), "Is 礼拜二");
        // 数字+中文是正常写法，不该插空格
        assert_eq!(space_cjk_latin("第2种"), "第2种");
        assert_eq!(space_cjk_latin("2026年11月"), "2026年11月");
        // 词里含字母才算英文词：MP3音乐 要插，3月 不插
        assert_eq!(space_cjk_latin("MP3音乐"), "MP3 音乐");
    }

    #[test]
    fn end_to_end_mixed_language_sentence() {
        // 热词修好"礼拜二"后的真实输出形态（ASR 原文没有中英之间的空格）
        assert_eq!(
            default_pp().process("昨天是 Monday Today Is礼拜二 The Day After Tomorrow是星期三"),
            "昨天是 Monday Today Is 礼拜二 The Day After Tomorrow 是星期三。"
        );
    }
}
