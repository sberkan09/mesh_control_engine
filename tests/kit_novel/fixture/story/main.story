# The kit's test story: a novel run (start) and an NPC conversation (mira).
@label start
The harbour is quiet.
Mira: Morning.
@wave 2
Mira:
  > "Wave back" {kind} -> start_wave
  > "Walk on" -> start_end
@label start_wave
@set trust +5
@ending letter
@end
@label start_end
Note: nobody waved.
@ending quiet
@end

@label mira
Mira: Hello there. #greet
@set trust +1
Mira:
  > "Ask about the boat" -> mira_boat
  > "Show the letter" [has_letter] -> mira_letter
  >? "Tell her the truth" [trust >= 2 and has_letter] -> mira_truth
  > "Leave" -> mira_bye
@label mira_boat
Mira(warm): It sails at dawn.
@flag asked_boat
@return
@label mira_letter
Yusuf: That's her brother's hand.
@ending letter
@return
@label mira_truth
Mira: Then you know.
@return
@label mira_bye
@return
@label theatre
@cutscene intro
@end
