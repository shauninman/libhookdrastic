all:
	@echo "# Screenshots\n" > README.md
	@for file in *.png; do \
		printf '<img width=240 src="%s" /> \n' "$$file" >> README.md; \
	done