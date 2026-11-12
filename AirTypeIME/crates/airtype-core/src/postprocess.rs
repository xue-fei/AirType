//! 文本后处理：口头词、空白、全角字符、末尾标点。
//! 纯字符串处理，不依赖任何平台 API。

use crate::config::PostProcessConfig;

const FILLERS: &[&str] = &["嗯", "啊", "呃", "这个", "那个", "然后", "就是说", "就是"];
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
        if self.cfg.collapse_whitespace {
            s = remove_cjk_spaces(&s);
        }
        if self.cfg.ensure_punctuation {
            s = ensure_punctuation(&s);
        }
        s.trim().to_string()
    }
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
        };
        assert_eq!(PostProcessor::new(cfg).process("嗯 你好"), "嗯 你好");
    }

    #[test]
    fn empty_input_stays_empty() {
        assert_eq!(default_pp().process("   "), "");
    }
}
