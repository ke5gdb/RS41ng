<template>
  <FieldShell :field="field" :input-id="inputId" :errors="errors">
    <input
      :id="inputId"
      type="number"
      :value="modelValue ?? ''"
      :min="field.validation?.min"
      :max="field.validation?.max"
      step="any"
      :class="[FIELD_CONTROL_CLASS, 'w-full']"
      @change="onInput"
    />
  </FieldShell>
</template>

<script setup lang="ts">
import FieldShell from "./FieldShell.vue";
import { useField, FIELD_CONTROL_CLASS, type FieldProps } from "@/composables/useField";

const props = defineProps<FieldProps>();
const emit = defineEmits<{ "update:modelValue": [value: number | string] }>();
const { inputId } = useField(props);

function onInput(e: Event) {
  const raw = (e.target as HTMLInputElement).value;
  const n = parseFloat(raw);
  emit("update:modelValue", isNaN(n) ? raw : n);
}
</script>
