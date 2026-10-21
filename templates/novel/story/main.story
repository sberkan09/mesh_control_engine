# @TITLE@: the script. Edit it and rebuild (mce build compiles it; mce story compile --check only checks).
# The syntax is in the user guide's "Novels and dialogue" chapter; story.json declares the names.
@scene start bg=room time=day
@show guide center
Guide: Welcome. This is the first page of your novel.
Guide:
  > "Tell me more" -> more
  > "Let me write" -> done

@label more
@flag curious
Guide: Lines are paragraphs, choices jump to labels, and flags and counters remember what happened.
@jump done

@label done
@hide guide
A line with no speaker is narration.
@ending first_page
@end
