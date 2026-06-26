
## 2024-05-18 - Missing keyboard sounds on custom iOS accessory views
**Learning:** Custom UIInputView accessory bars on iOS do not automatically play native keyboard click sounds, which can feel broken to users who rely on audio feedback. They must explicitly conform to UIInputViewAudioFeedback (returning true for enableInputClicksWhenVisible) and manually trigger UIDevice.current.playInputClick() on each key press.
**Action:** Whenever implementing custom keyboard buttons or accessory views on iOS, implement UIInputViewAudioFeedback and explicitly trigger playInputClick().
